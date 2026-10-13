#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "audio.h"
#include "capture_ctrl.h"
#include "log.h"
#include "sensor.h"
#include "spec.h"
#include "state.h"
#include "uds.h"
#include "udev_cameras.h"

namespace {

constexpr const char* kDefaultConf = "/etc/watchdog/cameras.conf";
constexpr const char* kDefaultFanConf = "/etc/watchdog/fan.conf";
constexpr const char* kDefaultSounds = "/etc/watchdog/sounds";
constexpr const char* kDefaultSock = "/tmp/watchdog.sock";
constexpr const char* kDefaultAlsa = "plughw:CARD=rockchipes8388,DEV=0";
constexpr const char* kDefaultCaptureSock = "/tmp/unified_capture.sock";
constexpr const char* kDefaultShutdownCmd = "systemctl poweroff";
constexpr const char* kShutdownVoice = "关机成功.wav";
constexpr int kCaptureStopTimeoutMs = 15000;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s                          config=%s\n"
                 "  %s <conf> [<sounds_dir> [<alsa_dev>]]\n"
                 "  %s --scan                    list present USB devices\n"
                 "  %s --help\n",
                 argv0, kDefaultConf, argv0, argv0, argv0);
}

int cmd_scan() {
    auto devs = enumerate_usb_devices();
    std::printf("present USB devices (%zu):\n", devs.size());
    for (const auto& d : devs) {
        std::printf("  vid=%04x pid=%04x product=[%s] serial=[%s] syspath=%s\n",
                    d.vid, d.pid, d.product.c_str(), d.serial.c_str(),
                    d.syspath.c_str());
    }
    return 0;
}

void ensure_speaker_audible() {
    if (std::system("amixer -c 0 sset Speaker 100% unmute >/dev/null 2>&1") != 0) {
        log_warn("amixer: Speaker unmute failed (continuing)");
    }
    if (std::system("amixer -c 0 sset PCM 100% unmute >/dev/null 2>&1") != 0) {
        log_warn("amixer: PCM unmute failed (continuing)");
    }
}

// Alarm thread: consumes recover voices and plays alarm voices for every
// missing ∧ !acked camera, repeating until the camera returns or is silenced.
void run_alarm_loop(State& state, const SoundBank& bank, AudioPlayer& player,
                    const std::atomic<bool>& running) {
    std::set<std::string> prev_alarming;
    while (running.load()) {
        state.take_cancel();  // consume stale wake/interrupt flags

        // One-shot "已恢复" voices. Play each name at most once per drain, and
        // only if the camera is *still* present when we get to it — a fast
        // re-unplug between the add event and this drain must not announce a
        // recovery (the alarm path below will pick it up instead).
        auto recovers = state.take_recover_queue();
        std::set<std::string> announced;
        for (const auto& name : recovers) {
            if (!running.load()) break;
            if (!announced.insert(name).second) continue;
            const std::string* wav_name = state.recover_wav_for(name);
            if (!wav_name) continue;
            if (!state.is_present(name)) continue;
            const WavData* wav = bank.find(*wav_name);
            if (!wav) {
                log_warn("recover sound missing for %s (file %s)", name.c_str(),
                         wav_name->c_str());
                continue;
            }
            log_info("recover name=%s file=%s", name.c_str(),
                     wav_name->c_str());
            player.play(*wav, state.cancel_flag());
            state.take_cancel();
        }

        // Alarm targets = missing ∧ !acked.
        std::vector<std::string> targets = state.alarming_specs();
        std::set<std::string> cur(targets.begin(), targets.end());
        for (const auto& t : targets) {
            if (prev_alarming.count(t) == 0) {
                log_info("ALARM start name=%s", t.c_str());
            }
        }
        for (const auto& t : prev_alarming) {
            if (cur.count(t) == 0) {
                log_info("ALARM stop name=%s", t.c_str());
            }
        }
        prev_alarming = std::move(cur);

        if (!targets.empty()) {
            for (const auto& name : targets) {
                if (!running.load()) break;
                const std::string* wav_name = state.alarm_wav_for(name);
                if (!wav_name) continue;
                const WavData* wav = bank.find(*wav_name);
                if (!wav) {
                    log_warn("alarm sound missing for %s (file %s) -> journald only",
                             name.c_str(), wav_name->c_str());
                    continue;
                }
                if (!player.play(*wav, state.cancel_flag())) {
                    // Cancelled (stop_alarm / new unplug) or ALSA error: stop
                    // this round here; the inter-round pause below still runs so
                    // a persistently failing ALSA device cannot busy-loop.
                    break;
                }
            }
            // Inter-round pause ~1.2 s (design: repeat every 2~3 s). Always
            // applied, but a pending cancel flag makes it exit immediately so a
            // new unplug or stop_alarm still takes effect with no extra delay.
            for (int i = 0; i < 6 && running.load() &&
                            !state.cancel_flag().load();
                 ++i) {
                usleep(200000);
            }
        } else {
            usleep(200000);
        }
    }
    log_info("alarm loop exit");
}

// Udev thread: watches add/remove of usb_device nodes and updates State.
void run_udev_loop(UdevMonitor& mon, State& state,
                   const std::atomic<bool>& running) {
    while (running.load()) {
        struct pollfd pfd {};
        pfd.fd = mon.fd();
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, 250);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pfd.revents & POLLIN) mon.process_events(state);
    }
    log_info("udev loop exit");
}

// Ordered shutdown after the control loop has stopped and all worker threads
// have joined (so the alarm thread can no longer contend for the speaker):
//   1. stop the capture session and wait for it to finalize on disk,
//   2. play "关机成功" to completion,
//   3. power the device off.
// Steps 1 and 2 are best-effort: any failure is logged and we still power off,
// so a missing socket or WAV can never leave the device unable to shut down.
void perform_shutdown(const SoundBank& bank, AudioPlayer& player,
                      const std::string& capture_sock,
                      const std::string& shutdown_cmd) {
    log_info("shutdown: stopping capture if running");
    switch (request_capture_stop(capture_sock, kCaptureStopTimeoutMs)) {
        case CaptureStopResult::stopped:
            log_info("shutdown: capture stopped, data finalized");
            break;
        case CaptureStopResult::not_running:
            log_info("shutdown: capture not running, skipping stop");
            break;
        case CaptureStopResult::unreachable:
            log_warn("shutdown: capture unreachable, proceeding anyway");
            break;
    }

    const WavData* wav = bank.find(kShutdownVoice);
    if (wav) {
        std::atomic<bool> no_cancel{false};
        log_info("shutdown: playing %s", kShutdownVoice);
        if (!player.play(*wav, no_cancel)) {
            log_warn("shutdown: voice playback failed, proceeding anyway");
        }
    } else {
        log_warn("shutdown: voice %s missing, proceeding anyway", kShutdownVoice);
    }

    log_info("shutdown: powering off via [%s]", shutdown_cmd.c_str());
    const int status = std::system(shutdown_cmd.c_str());
    if (status != 0) {
        log_warn("shutdown: command returned status %d", status);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string a1 = argv[1];
        if (a1 == "--help" || a1 == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (a1 == "--scan") {
            return cmd_scan();
        }
    }

    const std::string conf_path = argc > 1 ? argv[1] : kDefaultConf;
    const std::string sounds_dir = argc > 2 ? argv[2] : kDefaultSounds;
    const std::string alsa_dev = argc > 3 ? argv[3] : kDefaultAlsa;

    // Shutdown wiring. The env overrides let tests inject a fake capture socket
    // and a harmless shutdown command instead of really powering the machine off.
    auto env_or = [](const char* name, const char* dflt) -> std::string {
        const char* v = std::getenv(name);
        return std::string((v && *v) ? v : dflt);
    };
    const std::string capture_sock =
        env_or("WATCHDOG_CAPTURE_SOCK", kDefaultCaptureSock);
    const std::string shutdown_cmd =
        env_or("WATCHDOG_SHUTDOWN_CMD", kDefaultShutdownCmd);

    std::string err;
    std::vector<CameraSpec> specs = parse_cameras_conf(conf_path, err);
    if (specs.empty()) {
        log_error("config: %s", err.c_str());
        return 2;
    }
    log_info("config: loaded %zu camera spec(s) from %s", specs.size(),
             conf_path.c_str());

    const FanConfig fan_cfg = parse_fan_conf(kDefaultFanConf);
    std::vector<SensorChannel> sensors = {
        {"fan", fan_cfg.fan_alarm_wav, fan_cfg.fan_recover_wav},
        {"overtemp", fan_cfg.overtemp_alarm_wav, fan_cfg.overtemp_recover_wav},
    };
    log_info("config: fan hwmon=%s thermal=%s overtemp=%dmC stall=%ds poll=%ds",
             fan_cfg.fan_hwmon_name.c_str(), fan_cfg.thermal_zone_type.c_str(),
             fan_cfg.overtemp_threshold_mc, fan_cfg.stall_seconds,
             fan_cfg.poll_seconds);

    // Register handlers before any thread starts so a SIGTERM/SIGINT in the
    // setup window is caught rather than killing the process outright.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    SoundBank bank;
    bank.add_from_specs(specs, sounds_dir);
    for (const auto& s : sensors) {
        bank.add(s.alarm_wav, sounds_dir);
        bank.add(s.recover_wav, sounds_dir);
    }
    bank.add(kShutdownVoice, sounds_dir);

    ensure_speaker_audible();
    log_info("alsa: using device %s", alsa_dev.c_str());
    AudioPlayer player(alsa_dev);

    State state(std::move(specs), std::move(sensors));

    // Arm the udev monitor *before* enumerating, so a removal that happens in
    // the window between enable_receiving() and the initial enumeration is
    // queued and applied below instead of being missed (which would leave a
    // stale "present" entry and no alarm until the next add/remove).
    UdevMonitor mon;
    if (!mon.init()) {
        log_error("udev: monitor init failed");
        return 4;
    }

    // Startup enumeration -> initial present set.
    {
        auto devs = enumerate_usb_devices();
        int matched = 0;
        for (const auto& d : devs) {
            const int idx = match_first_spec(state.specs(), d);
            if (idx >= 0) {
                state.seed_present(static_cast<size_t>(idx), d.syspath);
                ++matched;
                log_info("present name=%s vid=%04x pid=%04x product=[%s] "
                         "serial=[%s]",
                         state.specs()[static_cast<size_t>(idx)].name.c_str(),
                         d.vid, d.pid, d.product.c_str(), d.serial.c_str());
            }
        }
        // Apply any add/remove events that queued up while we were enumerating.
        mon.process_events(state);
        const auto missing = state.missing_specs();
        log_info("startup: matched %d device(s), %zu channel(s) missing",
                 matched, missing.size());
        for (const auto& n : missing) {
            log_warn("startup missing -> will alarm: %s", n.c_str());
        }
    }

    UdsServer server(kDefaultSock);
    if (!server.open()) {
        return 3;
    }

    std::atomic<bool> running{true};
    std::thread udev_thr(run_udev_loop, std::ref(mon), std::ref(state),
                         std::ref(running));
    std::thread alarm_thr(run_alarm_loop, std::ref(state), std::ref(bank),
                          std::ref(player), std::ref(running));
    std::thread fan_thr(run_fan_loop, std::ref(state), std::cref(fan_cfg),
                        std::ref(running));

    log_info("watchdog running (socket=%s)", kDefaultSock);

    std::atomic<bool> shutdown_requested{false};
    const auto handler = [&state, &shutdown_requested](
                             const std::string& cmd) -> std::string {
        if (cmd == "status") {
            return state.status_json();
        }
        if (cmd == "stop_alarm") {
            state.ack_all_missing();
            log_info("command=stop_alarm: silenced all currently missing");
            return "{\"ok\":true}";
        }
        if (cmd == "stop_me") {
            if (shutdown_requested.exchange(true)) {
                log_info("command=stop_me: shutdown already in progress");
            } else {
                log_info("command=stop_me: shutdown requested");
            }
            return "{\"ok\":true,\"stopping\":true}";
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    };

    while (!g_stop && running.load() && !shutdown_requested.load()) {
        struct pollfd fds[1];
        fds[0].fd = server.fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int r = poll(fds, 1, 250);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r > 0) {
            if (fds[0].revents & POLLIN) server.serve_one(handler);
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        }
    }

    log_info("shutdown: stopping threads");
    running.store(false);
    state.request_cancel();  // interrupt any in-flight voice
    server.close();
    if (udev_thr.joinable()) udev_thr.join();
    if (alarm_thr.joinable()) alarm_thr.join();
    if (fan_thr.joinable()) fan_thr.join();

    if (shutdown_requested.load()) {
        perform_shutdown(bank, player, capture_sock, shutdown_cmd);
    }

    log_info("watchdog stopped");
    return 0;
}
