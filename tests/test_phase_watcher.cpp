// Host integration test: PhaseWatcher against a fake unified_capture UDS
// server and a fake VoiceSink. Exercises the F-05 prompt/handshake, button
// feedback, dedupe, missing-WAV degradation and handshake retry paths.

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phase_watcher.h"
#include "playback_arbiter.h"
#include "state.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using namespace std::chrono_literals;
using Handler = std::function<std::string(const std::string&)>;

class FakeCaptureServer {
public:
    ~FakeCaptureServer() { stop(); }

    bool start(const std::string& path, Handler handler) {
        path_ = path;
        handler_ = std::move(handler);
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) return false;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            return false;
        }
        if (::listen(fd_, 16) < 0) return false;
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mu_);
        return commands_;
    }

    int count_prefix(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mu_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.rfind(prefix, 0) == 0) ++n;
        }
        return n;
    }

    bool wait_for_prefix(const std::string& prefix, int count, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            int n = 0;
            for (const auto& c : commands_) {
                if (c.rfind(prefix, 0) == 0) ++n;
            }
            return n >= count;
        });
    }

private:
    void loop() {
        while (running_.load()) {
            struct pollfd pfd {};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int r = poll(&pfd, 1, 100);
            if (r <= 0) continue;
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) continue;

            std::string request;
            char buf[256];
            while (request.size() < 4096) {
                ssize_t n = ::read(client, buf, sizeof(buf));
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;
                request.append(buf, static_cast<size_t>(n));
                if (request.find('\n') != std::string::npos) break;
            }
            while (!request.empty() &&
                   (request.back() == '\n' || request.back() == '\r')) {
                request.pop_back();
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                commands_.push_back(request);
            }
            cv_.notify_all();

            std::string response = handler_ ? handler_(request) : std::string();
            if (!response.empty()) {
                response.push_back('\n');
                const char* ptr = response.data();
                size_t remaining = response.size();
                while (remaining > 0) {
                    ssize_t w = ::write(client, ptr, remaining);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    ptr += w;
                    remaining -= static_cast<size_t>(w);
                }
            }
            ::close(client);
        }
    }

    std::string path_;
    Handler handler_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> commands_;
};

class FakeSink : public VoiceSink {
public:
    PlayResult play(const WavData& wav, const std::atomic<bool>&,
                    const std::atomic<bool>*) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++plays_;
            last_source_ = wav.source;
        }
        cv_.notify_all();
        return PlayResult::played;
    }

    int plays() const {
        std::lock_guard<std::mutex> lock(mu_);
        return plays_;
    }

    std::string last_source() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_source_;
    }

    bool wait_for_plays(int n, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return plays_ >= n; });
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int plays_ = 0;
    std::string last_source_;
};

// Sink whose first `blocking_calls` playbacks block until preempted/cancelled;
// later playbacks return immediately. Used to verify interruption semantics.
class ScriptedSink : public VoiceSink {
public:
    struct Outcome {
        std::string source;
        PlayResult result;
    };

    explicit ScriptedSink(int blocking_calls, int max_block_ms = 0)
        : blocking_calls_(blocking_calls), max_block_ms_(max_block_ms) {}

    PlayResult play(const WavData& wav, const std::atomic<bool>& cancel,
                    const std::atomic<bool>* preempt) override {
        int index = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            index = ++calls_;
            started_.push_back(wav.source);
        }
        cv_.notify_all();

        if (index > blocking_calls_) {
            std::lock_guard<std::mutex> lock(mu_);
            completed_.push_back(wav.source);
            outcomes_.push_back({wav.source, PlayResult::played});
            cv_.notify_all();
            return PlayResult::played;
        }

        PlayResult result = PlayResult::error;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(max_block_ms_);
        for (;;) {
            if (cancel.load()) {
                result = PlayResult::cancelled;
                break;
            }
            if (preempt && preempt->load()) {
                result = PlayResult::preempted;
                break;
            }
            if (max_block_ms_ > 0 &&
                std::chrono::steady_clock::now() >= deadline) {
                result = PlayResult::played;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            outcomes_.push_back({wav.source, result});
        }
        cv_.notify_all();
        return result;
    }

    bool wait_for_started(int n, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return static_cast<int>(started_.size()) >= n; });
    }

    bool wait_for_completed(int n, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [&] { return static_cast<int>(completed_.size()) >= n; });
    }

    std::vector<std::string> started() const {
        std::lock_guard<std::mutex> lock(mu_);
        return started_;
    }
    std::vector<std::string> completed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return completed_;
    }
    std::vector<Outcome> outcomes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return outcomes_;
    }

    bool wait_for_outcomes(int n, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return static_cast<int>(outcomes_.size()) >= n; });
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int blocking_calls_ = 0;
    int max_block_ms_ = 0;
    int calls_ = 0;
    std::vector<std::string> started_;
    std::vector<std::string> completed_;
    std::vector<Outcome> outcomes_;
};

std::string make_temp_dir() {
    char tmpl[] = "/tmp/wd_sounds_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

std::string temp_sock_path(const char* tag) {
    return "/tmp/wd_" + std::string(tag) + "_" + std::to_string(::getpid()) +
           ".sock";
}

void append_u16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void append_u32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

bool write_test_wav(const std::string& path) {
    const std::string data(441 * 4, '\0');  // 10 ms stereo S16_LE
    std::string blob;
    blob += "RIFF";
    append_u32(blob, static_cast<uint32_t>(36 + data.size()));
    blob += "WAVE";
    blob += "fmt ";
    append_u32(blob, 16);
    append_u16(blob, 1);       // PCM
    append_u16(blob, 2);       // channels
    append_u32(blob, 44100);   // rate
    append_u32(blob, 44100u * 4u);
    append_u16(blob, 4);       // block align
    append_u16(blob, 16);      // bits
    blob += "data";
    append_u32(blob, static_cast<uint32_t>(data.size()));
    blob += data;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    std::fclose(f);
    return ok;
}

const char* kZeroDoneJson =
    "{\"ok\":true,\"phase\":\"zero_done_speaking\",\"phase_version\":1,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":0,"
    "\"last_event_id\":\"\",\"last_source\":\"\",\"last_action\":\"\","
    "\"last_result\":\"\",\"keys\":\"ready\",\"blocked\":false}";

const char* kPreviewBaselineJson =
    "{\"ok\":true,\"phase\":\"preview_ready\",\"phase_version\":0,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":0,"
    "\"last_event_id\":\"\",\"last_source\":\"\",\"last_action\":\"\","
    "\"last_result\":\"\",\"keys\":\"ready\",\"blocked\":false}";

const char* kPreviewButtonJson =
    "{\"ok\":true,\"phase\":\"preview_ready\",\"phase_version\":1,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":0,"
    "\"last_event_id\":\"evt-1\",\"last_source\":\"gpio\","
    "\"last_action\":\"zero_start\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}";

const char* kZeroWaitButtonJson =
    "{\"ok\":true,\"phase\":\"zero_wait\",\"phase_version\":2,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":0,"
    "\"last_event_id\":\"evt-1\",\"last_source\":\"gpio\","
    "\"last_action\":\"zero_start\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}";

const char* kPreviewCancelJson =
    "{\"ok\":true,\"phase\":\"preview_ready\",\"phase_version\":2,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":0,"
    "\"last_event_id\":\"evt-2\",\"last_source\":\"gpio\","
    "\"last_action\":\"zero_cancel\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}";

const char* kAnnounceOkJson =
    "{\"ok\":true,\"phase\":\"prep_ready\",\"phase_version\":2,"
    "\"event_id\":\"x\",\"source\":\"ui\",\"duplicate\":false}";

const char* kPrepDoneJson =
    "{\"ok\":true,\"phase\":\"prep_done\",\"phase_version\":1,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":130000,"
    "\"last_event_id\":\"evt-3\",\"last_source\":\"gpio\","
    "\"last_action\":\"prep_finish\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}";

const char* kCalibrationCompleteJson =
    "{\"ok\":true,\"phase\":\"calibration_complete\",\"phase_version\":2,"
    "\"task_id\":\"t1\",\"threshold_s\":120,\"prep_elapsed_ms\":130000,"
    "\"last_event_id\":\"evt-3\",\"last_source\":\"gpio\","
    "\"last_action\":\"prep_finish\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}";

// Runs a watcher until the caller stops it.
struct WatcherRun {
    std::atomic<bool> running{true};
    PhaseWatcher watcher;

    WatcherRun(PlaybackArbiter& arbiter, const SoundBank& bank, State& state,
               std::string sock)
        : watcher(arbiter, bank, state, running, std::move(sock)) {}
    ~WatcherRun() {
        running.store(false);
        watcher.request_stop();
        if (thread.joinable()) thread.join();
    }
    void start() { thread = std::thread([this] { watcher.run(); }); }
    std::thread thread;
};

}  // namespace

int main() {
    // ---- 1. zero_done_speaking: one prompt, one handshake ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroPromptWav));

        SoundBank bank;
        bank.add(phase_voice::kZeroPromptWav, dir);
        CHECK(bank.find(phase_voice::kZeroPromptWav) != nullptr);
        State state({}, {});
        FakeSink sink;
        PlaybackArbiter arbiter(sink);

        const std::string sock = temp_sock_path("prompt");
        FakeCaptureServer server;
        CHECK(server.start(sock, [](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") return kZeroDoneJson;
            if (cmd.rfind("announce_done:", 0) == 0) return kAnnounceOkJson;
            return "{\"ok\":false,\"error\":\"unknown\"}";
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(server.wait_for_prefix("announce_done:", 1, 3000));
            std::this_thread::sleep_for(600ms);
        }
        server.stop();

        CHECK(sink.plays() == 1);
        CHECK(sink.last_source().find(phase_voice::kZeroPromptWav) !=
              std::string::npos);
        CHECK(server.count_prefix("announce_done:") == 1);
        for (const auto& cmd : server.commands()) {
            if (cmd.rfind("announce_done:", 0) == 0) {
                CHECK(cmd.rfind("announce_done:ui:watchdog-", 0) == 0);
            }
        }
        ::unlink((dir + "/" + phase_voice::kZeroPromptWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 2. gpio button event plays exactly once ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroStartWav));

        SoundBank bank;
        bank.add(phase_voice::kZeroStartWav, dir);
        State state({}, {});
        FakeSink sink;
        PlaybackArbiter arbiter(sink);

        const std::string sock = temp_sock_path("button");
        FakeCaptureServer server;
        std::atomic<int> polls{0};
        CHECK(server.start(sock, [&polls](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") {
                return ++polls == 1 ? kPreviewBaselineJson : kPreviewButtonJson;
            }
            return "{\"ok\":false,\"error\":\"unknown\"}";
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(sink.wait_for_plays(1, 3000));
            std::this_thread::sleep_for(700ms);  // several more polls
        }
        server.stop();

        CHECK(sink.plays() == 1);
        CHECK(sink.last_source().find(phase_voice::kZeroStartWav) !=
              std::string::npos);
        CHECK(polls.load() >= 3);
        ::unlink((dir + "/" + phase_voice::kZeroStartWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 3. missing prompt WAV still completes the handshake ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        SoundBank bank;
        bank.add(phase_voice::kZeroPromptWav, dir);  // logs missing
        CHECK(bank.find(phase_voice::kZeroPromptWav) == nullptr);

        State state({}, {});
        FakeSink sink;
        PlaybackArbiter arbiter(sink);
        const std::string sock = temp_sock_path("missing");
        FakeCaptureServer server;
        CHECK(server.start(sock, [](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") return kZeroDoneJson;
            if (cmd.rfind("announce_done:", 0) == 0) return kAnnounceOkJson;
            return "{\"ok\":false,\"error\":\"unknown\"}";
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(server.wait_for_prefix("announce_done:", 1, 3000));
            std::this_thread::sleep_for(300ms);
        }
        server.stop();

        CHECK(sink.plays() == 0);
        CHECK(server.count_prefix("announce_done:") == 1);
        ::rmdir(dir.c_str());
    }

    // ---- 4. failed announce_done is retried with the same event id ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroPromptWav));
        SoundBank bank;
        bank.add(phase_voice::kZeroPromptWav, dir);

        State state({}, {});
        FakeSink sink;
        PlaybackArbiter arbiter(sink);
        const std::string sock = temp_sock_path("retry");
        FakeCaptureServer server;
        std::atomic<int> announce_calls{0};
        CHECK(server.start(sock, [&announce_calls](const std::string& cmd) {
            if (cmd == "phase_status") return std::string(kZeroDoneJson);
            if (cmd.rfind("announce_done:", 0) == 0) {
                // Drop the first response; the client must retry.
                if (++announce_calls == 1) return std::string();
                return std::string(kAnnounceOkJson);
            }
            return std::string("{\"ok\":false,\"error\":\"unknown\"}");
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(server.wait_for_prefix("announce_done:", 2, 4000));
            std::this_thread::sleep_for(400ms);
        }
        server.stop();

        CHECK(announce_calls.load() == 2);
        const auto cmds = server.commands();
        std::vector<std::string> announce;
        for (const auto& cmd : cmds) {
            if (cmd.rfind("announce_done:", 0) == 0) announce.push_back(cmd);
        }
        CHECK(announce.size() == 2);
        if (announce.size() == 2) CHECK(announce[0] == announce[1]);
        ::unlink((dir + "/" + phase_voice::kZeroPromptWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 5. a key press interrupts a playing prompt; handshake still sent ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroPromptWav));
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroStartWav));

        SoundBank bank;
        bank.add(phase_voice::kZeroPromptWav, dir);
        bank.add(phase_voice::kZeroStartWav, dir);
        State state({}, {});
        ScriptedSink sink(1);  // the prompt blocks until preempted
        PlaybackArbiter arbiter(sink);

        const std::string sock = temp_sock_path("interrupt");
        FakeCaptureServer server;
        std::atomic<bool> send_button{false};
        std::atomic<int> phase_polls{0};
        CHECK(server.start(sock, [&](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") {
                ++phase_polls;
                return send_button.load() ? std::string(kZeroWaitButtonJson)
                                          : std::string(kZeroDoneJson);
            }
            if (cmd.rfind("announce_done:", 0) == 0) {
                return std::string(kAnnounceOkJson);
            }
            return std::string("{\"ok\":false,\"error\":\"unknown\"}");
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(sink.wait_for_started(1, 3000));
            // While the prompt is playing the polling thread must keep polling.
            const int polls_before = phase_polls.load();
            std::this_thread::sleep_for(400ms);
            CHECK(phase_polls.load() > polls_before);

            send_button.store(true);
            CHECK(sink.wait_for_completed(1, 4000));
            std::this_thread::sleep_for(300ms);
        }
        server.stop();

        const auto outcomes = sink.outcomes();
        CHECK(outcomes.size() >= 2);
        if (outcomes.size() >= 1) {
            CHECK(outcomes[0].source.find(phase_voice::kZeroPromptWav) !=
                  std::string::npos);
            CHECK(outcomes[0].result == PlayResult::preempted);
        }
        const auto completed = sink.completed();
        CHECK(completed.size() == 1);
        if (completed.size() == 1) {
            CHECK(completed[0].find(phase_voice::kZeroStartWav) !=
                  std::string::npos);
        }
        CHECK(server.count_prefix("announce_done:") == 1);
        CHECK(phase_polls.load() >= 3);
        ::unlink((dir + "/" + phase_voice::kZeroPromptWav).c_str());
        ::unlink((dir + "/" + phase_voice::kZeroStartWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 6. a second key press interrupts the first button voice ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroStartWav));
        CHECK(write_test_wav(dir + "/" + phase_voice::kZeroCancelWav));

        SoundBank bank;
        bank.add(phase_voice::kZeroStartWav, dir);
        bank.add(phase_voice::kZeroCancelWav, dir);
        State state({}, {});
        ScriptedSink sink(1);  // the first button blocks
        PlaybackArbiter arbiter(sink);

        const std::string sock = temp_sock_path("double");
        FakeCaptureServer server;
        std::atomic<int> polls{0};
        std::atomic<bool> send_second{false};
        CHECK(server.start(sock, [&](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") {
                if (++polls == 1) return std::string(kPreviewBaselineJson);
                return send_second.load() ? std::string(kPreviewCancelJson)
                                          : std::string(kPreviewButtonJson);
            }
            return std::string("{\"ok\":false,\"error\":\"unknown\"}");
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(sink.wait_for_started(1, 3000));
            send_second.store(true);
            CHECK(sink.wait_for_completed(1, 4000));
            std::this_thread::sleep_for(300ms);
        }
        server.stop();

        const auto started = sink.started();
        const auto completed = sink.completed();
        const auto outcomes = sink.outcomes();
        CHECK(started.size() == 2);
        CHECK(completed.size() == 1);
        if (completed.size() == 1) {
            CHECK(completed[0].find(phase_voice::kZeroCancelWav) !=
                  std::string::npos);
        }
        CHECK(outcomes.size() >= 2);
        if (outcomes.size() >= 1) {
            CHECK(outcomes[0].source.find(phase_voice::kZeroStartWav) !=
                  std::string::npos);
            CHECK(outcomes[0].result == PlayResult::preempted);
        }
        ::unlink((dir + "/" + phase_voice::kZeroStartWav).c_str());
        ::unlink((dir + "/" + phase_voice::kZeroCancelWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 7. calibration_complete voice is not cut off by later polls ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kCompleteWav));
        SoundBank bank;
        bank.add(phase_voice::kCompleteWav, dir);
        State state({}, {});
        // Blocks for 800 ms so the next 500 ms calibration_complete poll runs
        // while the voice is still playing. A stale request_preempt(prompt)
        // would cut it short; the fixed watcher lets it finish.
        ScriptedSink sink(1, 800);
        PlaybackArbiter arbiter(sink);

        const std::string sock = temp_sock_path("complete");
        FakeCaptureServer server;
        std::atomic<int> polls{0};
        CHECK(server.start(sock, [&polls](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") {
                return ++polls == 1 ? std::string(kPrepDoneJson)
                                    : std::string(kCalibrationCompleteJson);
            }
            return std::string("{\"ok\":false,\"error\":\"unknown\"}");
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            CHECK(sink.wait_for_started(1, 3000));
            CHECK(sink.wait_for_outcomes(1, 4000));
            std::this_thread::sleep_for(300ms);
        }
        server.stop();

        const auto outcomes = sink.outcomes();
        CHECK(outcomes.size() == 1);
        if (outcomes.size() == 1) {
            CHECK(outcomes[0].source.find(phase_voice::kCompleteWav) !=
                  std::string::npos);
            CHECK(outcomes[0].result == PlayResult::played);
        }
        CHECK(polls.load() >= 3);  // polling continued during playback
        ::unlink((dir + "/" + phase_voice::kCompleteWav).c_str());
        ::rmdir(dir.c_str());
    }

    // ---- 8. calibration_complete is skipped immediately under alarm ----
    {
        const std::string dir = make_temp_dir();
        CHECK(!dir.empty());
        CHECK(write_test_wav(dir + "/" + phase_voice::kCompleteWav));
        SoundBank bank;
        bank.add(phase_voice::kCompleteWav, dir);

        CameraSpec spec;
        spec.name = "cam";
        spec.alarm_wav = "a.wav";
        spec.recover_wav = "b.wav";
        State state({spec}, {});
        CHECK(!state.alarming_specs().empty());

        ScriptedSink sink(0);
        PlaybackArbiter arbiter(sink);
        const std::string sock = temp_sock_path("complete_alarm");
        FakeCaptureServer server;
        std::atomic<int> polls{0};
        CHECK(server.start(sock, [&polls](const std::string& cmd) -> std::string {
            if (cmd == "phase_status") {
                return ++polls == 1 ? std::string(kPrepDoneJson)
                                    : std::string(kCalibrationCompleteJson);
            }
            return std::string("{\"ok\":false,\"error\":\"unknown\"}");
        }));

        {
            WatcherRun run(arbiter, bank, state, sock);
            run.start();
            // Several calibration_complete polls happen while the alarm is
            // active. The worker must skip the completion voice at enqueue
            // time, not park in the 10 s alarm wait.
            CHECK(server.wait_for_prefix("phase_status", 4, 4000));
            // Silencing the alarm now must not release an already-parked
            // worker. If the completion voice had entered the 10 s wait it
            // would start playing once the alarm clears.
            state.ack_all_missing();
            CHECK(state.alarming_specs().empty());
            std::this_thread::sleep_for(900ms);
        }
        server.stop();

        CHECK(sink.started().empty());  // skipped immediately, no 10 s wait
        ::unlink((dir + "/" + phase_voice::kCompleteWav).c_str());
        ::rmdir(dir.c_str());
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_phase_watcher: ok\n");
    return 0;
}
