// Host tests for PlaybackArbiter priority / preemption / serialization.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "playback_arbiter.h"

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

class SlowSink : public VoiceSink {
public:
    explicit SlowSink(int duration_ms) : duration_ms_(duration_ms) {}

    PlayResult play(const WavData&, const std::atomic<bool>& cancel,
                    const std::atomic<bool>* preempt) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++plays_;
            cv_.notify_all();
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms_);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel.load()) return PlayResult::cancelled;
            if (preempt && preempt->load()) return PlayResult::preempted;
            std::this_thread::sleep_for(1ms);
        }
        if (cancel.load()) return PlayResult::cancelled;
        if (preempt && preempt->load()) return PlayResult::preempted;
        return PlayResult::played;
    }

    int plays() const {
        std::lock_guard<std::mutex> lock(mu_);
        return plays_;
    }

    bool wait_for_plays(int n, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return plays_ >= n; });
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int plays_ = 0;
    int duration_ms_ = 0;
};

WavData make_wav(const std::string& name) {
    WavData wav;
    wav.sample_rate = 44100;
    wav.channels = 2;
    wav.pcm.assign(4, 0);
    wav.source = name;
    return wav;
}

}  // namespace

int main() {
    const WavData wav = make_wav("x.wav");
    std::atomic<bool> no_cancel{false};

    // ---- basic serialized playback ----
    {
        SlowSink sink(0);
        PlaybackArbiter arbiter(sink);
        CHECK(arbiter.play(wav, no_cancel, VoicePriority::alarm, 0) ==
              PlayResult::played);
        CHECK(sink.plays() == 1);
        CHECK(arbiter.current_priority() == VoicePriority::alarm);
    }

    // ---- lower priority is skipped while an alarm is playing ----
    {
        SlowSink sink(400);
        PlaybackArbiter arbiter(sink);
        std::atomic<bool> cancel{false};
        std::thread alarm([&] { arbiter.play(wav, cancel, VoicePriority::alarm, 0); });
        CHECK(sink.wait_for_plays(1));
        const auto start = std::chrono::steady_clock::now();
        const PlayResult r = arbiter.play(wav, no_cancel, VoicePriority::prompt, 50);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(r == PlayResult::skipped);
        CHECK(elapsed >= 40ms);
        cancel.store(true);
        alarm.join();
    }

    // ---- a button preempts a flow prompt ----
    {
        SlowSink sink(400);
        PlaybackArbiter arbiter(sink);
        std::atomic<bool> cancel{false};
        PlayResult prompt_result = PlayResult::error;
        std::thread prompt([&] {
            prompt_result = arbiter.play(wav, cancel, VoicePriority::prompt, 0);
        });
        CHECK(sink.wait_for_plays(1));
        const PlayResult button =
            arbiter.play(wav, no_cancel, VoicePriority::button, 0);
        prompt.join();
        CHECK(button == PlayResult::played);
        CHECK(prompt_result == PlayResult::preempted);
        CHECK(sink.plays() == 2);
    }

    // ---- a newer button replaces the older button ----
    {
        SlowSink sink(400);
        PlaybackArbiter arbiter(sink);
        std::atomic<bool> cancel{false};
        PlayResult first = PlayResult::error;
        std::thread t([&] {
            first = arbiter.play(wav, cancel, VoicePriority::button, 0);
        });
        CHECK(sink.wait_for_plays(1));
        const PlayResult second =
            arbiter.play(wav, no_cancel, VoicePriority::button, 0);
        t.join();
        CHECK(first == PlayResult::preempted);
        CHECK(second == PlayResult::played);
        CHECK(sink.plays() == 2);
    }

    // ---- a key requested while a prompt waits for the lock wins ----
    // Regression for the "request_preempt while active_==false" window: the
    // alarm holds the speaker, the prompt blocks on the lock, then a button is
    // requested. The prompt must yield instead of starting ahead of the key.
    {
        SlowSink sink(500);
        PlaybackArbiter arbiter(sink);
        std::thread alarm([&] {
            arbiter.play(wav, no_cancel, VoicePriority::alarm, 0);
        });
        CHECK(sink.wait_for_plays(1));

        std::atomic<PlayResult> prompt_result{PlayResult::error};
        std::thread prompt([&] {
            prompt_result.store(
                arbiter.play(wav, no_cancel, VoicePriority::prompt, 3000));
        });
        std::this_thread::sleep_for(100ms);  // prompt is now blocked on the lock
        arbiter.request_preempt(VoicePriority::button);
        alarm.join();
        prompt.join();
        CHECK(prompt_result.load() == PlayResult::skipped);
        CHECK(sink.plays() == 1);  // only the alarm reached the speaker
    }

    // ---- a request that only loses to equal/lower priority is not blocked ----
    {
        SlowSink sink(0);
        PlaybackArbiter arbiter(sink);
        arbiter.request_preempt(VoicePriority::prompt);  // stale, lower priority
        CHECK(arbiter.play(wav, no_cancel, VoicePriority::button, 0) ==
              PlayResult::played);
    }

    // ---- shutdown preempts an alarm ----
    {
        SlowSink sink(400);
        PlaybackArbiter arbiter(sink);
        std::atomic<bool> cancel{false};
        PlayResult alarm_result = PlayResult::error;
        std::thread alarm([&] {
            alarm_result = arbiter.play(wav, cancel, VoicePriority::alarm, 0);
        });
        CHECK(sink.wait_for_plays(1));
        const PlayResult shutdown =
            arbiter.play(wav, no_cancel, VoicePriority::shutdown, 0);
        alarm.join();
        CHECK(shutdown == PlayResult::played);
        CHECK(alarm_result == PlayResult::preempted);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_playback_arbiter: ok\n");
    return 0;
}
