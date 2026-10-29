#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "audio.h"
#include "phase_state_view.h"
#include "phase_voice_policy.h"

class PlaybackArbiter;
class State;

// Polls unified_capture `phase_status` on its own thread while a separate
// voice worker performs playback and announce_done handshakes. Keeping I/O and
// playback off the polling thread lets a new key press preempt a long prompt.
class PhaseWatcher {
public:
    PhaseWatcher(PlaybackArbiter& arbiter, const SoundBank& bank, State& state,
                 const std::atomic<bool>& running, std::string capture_sock);
    ~PhaseWatcher();

    PhaseWatcher(const PhaseWatcher&) = delete;
    PhaseWatcher& operator=(const PhaseWatcher&) = delete;

    void run();
    // Wakes and joins the voice worker. Idempotent, so the owner may call it
    // explicitly before shutdown and again from the destructor.
    void request_stop();

private:
    enum class VoiceKind { button, prompt, complete };

    struct VoiceJob {
        std::string wav;
        PhaseStatus status;
    };

    bool sleep_ms(int ms);
    void handle_status(const PhaseStatus& status);

    // Polling thread -> worker mailbox.
    void enqueue_voice(VoiceKind kind, const std::string& wav,
                       const PhaseStatus& status);
    void request_handshake(int phase_version, const PhaseStatus& status);
    void drop_pending_prompt();
    bool prompt_superseded();

    // Voice worker.
    void voice_loop();
    void play_button(const VoiceJob& job);
    PlayResult play_flow_voice(const VoiceJob& job, const char* reason,
                               int max_wait_ms, bool is_prompt);
    void maybe_handshake(int phase_version, const PhaseStatus& status);
    HandshakeOutcome send_handshake(const std::string& event_id);

    void note_failure(const std::string& error);
    std::string make_event_id();
    void log_voice(const PhaseStatus& status, const std::string& voice,
                   const std::string& play, const std::string& reason) const;

    PlaybackArbiter& arbiter_;
    const SoundBank& bank_;
    State& state_;
    const std::atomic<bool>& running_;
    std::string capture_sock_;

    // Polling-thread policy state. The worker only touches policy_ through
    // policy_mu_; socket I/O never happens under this mutex.
    mutable std::mutex policy_mu_;
    PhaseVoicePolicy policy_;

    // Last phase seen by the polling thread, used to detect leaving
    // zero_done_speaking without touching the policy mutex.
    std::string observed_phase_;

    std::atomic<bool> stop_{false};

    // Voice worker mailbox: at most one job per kind; a newer job replaces the
    // previous one of the same kind.
    std::mutex mail_mu_;
    std::condition_variable mail_cv_;
    bool worker_stop_ = false;
    std::optional<VoiceJob> button_job_;
    std::optional<VoiceJob> prompt_job_;
    std::optional<VoiceJob> complete_job_;
    bool prompt_superseded_ = false;
    int handshake_requested_version_ = -1;
    PhaseStatus handshake_requested_status_;

    std::mutex join_mu_;
    std::thread worker_;

    bool failing_ = false;
    int failure_count_ = 0;
    int backoff_index_ = 0;
    std::chrono::steady_clock::time_point last_fail_log_{};

    // Worker-only handshake id bookkeeping.
    int handshake_version_ = -1;
    std::string handshake_event_id_;
    long long event_counter_ = 0;
};
