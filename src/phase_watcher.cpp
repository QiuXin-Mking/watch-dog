#include "phase_watcher.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "capture_client.h"
#include "log.h"
#include "playback_arbiter.h"
#include "state.h"

namespace {

constexpr int kConnectTimeoutMs = 500;
constexpr int kIoTimeoutMs = 500;
constexpr int kPromptAlarmWaitMs = 10000;
constexpr int kPromptAlarmPollMs = 200;
constexpr int kPromptQueueWaitMs = 3000;
constexpr int kSleepSliceMs = 50;

const char* play_result_name(PlayResult result) {
    switch (result) {
        case PlayResult::played: return "ok";
        case PlayResult::cancelled: return "cancelled";
        case PlayResult::preempted: return "preempted";
        case PlayResult::skipped: return "skipped";
        case PlayResult::error: return "error";
    }
    return "error";
}

const char* handshake_outcome_name(HandshakeOutcome outcome) {
    switch (outcome) {
        case HandshakeOutcome::success: return "ok";
        case HandshakeOutcome::invalid_phase: return "invalid_phase";
        case HandshakeOutcome::failed: return "failed";
    }
    return "failed";
}

}  // namespace

PhaseWatcher::PhaseWatcher(PlaybackArbiter& arbiter, const SoundBank& bank,
                           State& state, const std::atomic<bool>& running,
                           std::string capture_sock)
    : arbiter_(arbiter),
      bank_(bank),
      state_(state),
      running_(running),
      capture_sock_(std::move(capture_sock)) {
    worker_ = std::thread([this] { voice_loop(); });
}

PhaseWatcher::~PhaseWatcher() { request_stop(); }

void PhaseWatcher::request_stop() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(mail_mu_);
        worker_stop_ = true;
    }
    mail_cv_.notify_all();
    std::lock_guard<std::mutex> join_lock(join_mu_);
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool PhaseWatcher::sleep_ms(int ms) {
    int waited = 0;
    while (waited < ms) {
        if (!running_.load() || stop_.load()) return false;
        const int slice = std::min(kSleepSliceMs, ms - waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        waited += slice;
    }
    return running_.load() && !stop_.load();
}

void PhaseWatcher::log_voice(const PhaseStatus& status, const std::string& voice,
                             const std::string& play,
                             const std::string& reason) const {
    const std::string event =
        status.last_event_id.empty() ? "-" : status.last_event_id;
    const std::string action =
        status.last_action.empty() ? "-" : status.last_action;
    const std::string result =
        status.last_result.empty() ? "-" : status.last_result;
    log_info("phase voice: phase=%s phase_version=%d event_id=%s action=%s "
             "result=%s voice=%s play=%s reason=%s",
             status.phase.c_str(), status.phase_version, event.c_str(),
             action.c_str(), result.c_str(), voice.c_str(), play.c_str(),
             reason.c_str());
}

void PhaseWatcher::note_failure(const std::string& error) {
    ++failure_count_;
    const auto now = std::chrono::steady_clock::now();
    const bool due_to_time =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_fail_log_) >=
        std::chrono::seconds(30);
    const bool log_now = !failing_ || failure_count_ % 10 == 1 || due_to_time;
    if (log_now) {
        log_warn("phase watcher: poll failed (%d consecutive): %s",
                 failure_count_, error.c_str());
        last_fail_log_ = now;
    }
    failing_ = true;
}

std::string PhaseWatcher::make_event_id() {
    const long long epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return "watchdog-" + std::to_string(epoch_ms) + "-" +
           std::to_string(++event_counter_);
}

// ---------------------------------------------------------------------------
// Polling thread -> voice worker
// ---------------------------------------------------------------------------
void PhaseWatcher::enqueue_voice(VoiceKind kind, const std::string& wav,
                                 const PhaseStatus& status) {
    {
        std::lock_guard<std::mutex> lock(mail_mu_);
        VoiceJob job{wav, status};
        if (kind == VoiceKind::button) {
            // A physical key supersedes a flow prompt that has not started yet.
            prompt_job_.reset();
            prompt_superseded_ = true;
            button_job_ = std::move(job);
        } else if (kind == VoiceKind::prompt) {
            prompt_job_ = std::move(job);
        } else {
            complete_job_ = std::move(job);
        }
    }
    mail_cv_.notify_all();
}

void PhaseWatcher::request_handshake(int phase_version,
                                     const PhaseStatus& status) {
    {
        std::lock_guard<std::mutex> lock(mail_mu_);
        // A prompt still waiting in the mailbox will handshake when it ends;
        // do not let announce_done overtake the prompt audio.
        if (prompt_job_) return;
        handshake_requested_version_ = phase_version;
        handshake_requested_status_ = status;
    }
    mail_cv_.notify_all();
}

void PhaseWatcher::drop_pending_prompt() {
    std::lock_guard<std::mutex> lock(mail_mu_);
    prompt_job_.reset();
}

bool PhaseWatcher::prompt_superseded() {
    std::lock_guard<std::mutex> lock(mail_mu_);
    return prompt_superseded_;
}

void PhaseWatcher::handle_status(const PhaseStatus& status) {
    VoiceDecision decision;
    {
        std::lock_guard<std::mutex> lock(policy_mu_);
        decision = policy_.observe(status);
    }

    if (!decision.button_wav.empty()) {
        enqueue_voice(VoiceKind::button, decision.button_wav, status);
        arbiter_.request_preempt(VoicePriority::button);
    }
    if (!decision.zero_prompt_wav.empty()) {
        enqueue_voice(VoiceKind::prompt, decision.zero_prompt_wav, status);
        arbiter_.request_preempt(VoicePriority::prompt);
    }
    if (!decision.complete_wav.empty()) {
        enqueue_voice(VoiceKind::complete, decision.complete_wav, status);
        arbiter_.request_preempt(VoicePriority::prompt);
    }
    if (decision.handshake_pending && decision.zero_prompt_wav.empty()) {
        // A button won over (or is interrupting) the prompt: handshake now,
        // before the button voice, so feedback cannot delay the state machine.
        request_handshake(status.phase_version, status);
    }

    if (observed_phase_ == "zero_done_speaking" &&
        status.phase != "zero_done_speaking") {
        // Leaving zero_done_speaking: never start a stale prompt. An
        // already-playing prompt is preempted and still completes its
        // handshake in the worker. Do not preempt on every non-zero phase:
        // calibration_complete's own prompt (same priority) must finish.
        drop_pending_prompt();
        {
            std::lock_guard<std::mutex> lock(mail_mu_);
            handshake_requested_version_ = -1;
        }
        arbiter_.request_preempt(VoicePriority::prompt);
    }
    observed_phase_ = status.phase;
}

// ---------------------------------------------------------------------------
// Voice worker
// ---------------------------------------------------------------------------
void PhaseWatcher::play_button(const VoiceJob& job) {
    if (!state_.alarming_specs().empty()) {
        log_voice(job.status, "-", "skipped",
                  "button voice skipped: alarm active");
        return;
    }
    const WavData* wav = bank_.find(job.wav);
    if (!wav) {
        log_warn("sound: %s (missing/unusable WAV -> journald-only)",
                 job.wav.c_str());
        log_voice(job.status, "-", "skipped", "button voice missing/unusable");
        return;
    }
    const PlayResult result =
        arbiter_.play(*wav, state_.cancel_flag(), VoicePriority::button, 0);
    log_voice(job.status, job.wav, play_result_name(result), "button");
}

PlayResult PhaseWatcher::play_flow_voice(const VoiceJob& job, const char* reason,
                                         int max_wait_ms, bool is_prompt) {
    const WavData* wav = bank_.find(job.wav);
    if (!wav) {
        log_warn("sound: %s (missing/unusable WAV -> journald-only)",
                 job.wav.c_str());
        log_voice(job.status, "-", "skipped",
                  std::string(reason) + " missing, handshake only");
        return PlayResult::skipped;
    }

    // A non-prompt voice (calibration_complete) has no handshake to protect:
    // skip it immediately under alarm or when a key is waiting, instead of
    // holding the single voice worker for 10 s.
    if (!is_prompt &&
        (prompt_superseded() || !state_.alarming_specs().empty())) {
        log_voice(job.status, "-", "skipped",
                  std::string(reason) +
                      (prompt_superseded() ? " superseded by key"
                                           : " skipped: alarm active"));
        return PlayResult::skipped;
    }

    // Calibration prompts never interrupt an active alarm: wait up to 10 s,
    // then skip the voice but still complete the handshake. A new key press
    // aborts the wait so its feedback is not delayed.
    int waited = 0;
    while (!state_.alarming_specs().empty()) {
        if (is_prompt && prompt_superseded()) {
            log_voice(job.status, "-", "skipped",
                      std::string(reason) + " superseded by key");
            return PlayResult::skipped;
        }
        if (waited >= kPromptAlarmWaitMs) {
            log_voice(job.status, "-", "skipped",
                      std::string(reason) + " skipped: alarm active");
            return PlayResult::skipped;
        }
        if (!sleep_ms(kPromptAlarmPollMs)) return PlayResult::skipped;
        waited += kPromptAlarmPollMs;
    }
    if (!running_.load() || stop_.load()) return PlayResult::skipped;
    if (is_prompt && prompt_superseded()) {
        log_voice(job.status, "-", "skipped",
                  std::string(reason) + " superseded by key");
        return PlayResult::skipped;
    }

    const PlayResult result =
        arbiter_.play(*wav, state_.cancel_flag(), VoicePriority::prompt,
                      max_wait_ms);
    std::string why = reason;
    if (result == PlayResult::skipped) {
        if (is_prompt && prompt_superseded()) {
            why += " superseded by key";
        } else {
            why += state_.alarming_specs().empty() ? " skipped: audio busy"
                                                   : " skipped: alarm active";
        }
    } else if (result == PlayResult::preempted) {
        why += " interrupted";
    } else if (result == PlayResult::cancelled) {
        why += " cancelled";
    }
    log_voice(job.status, job.wav, play_result_name(result), why);
    return result;
}

HandshakeOutcome PhaseWatcher::send_handshake(const std::string& event_id) {
    static const int kRetryDelaysMs[3] = {200, 500, 1000};
    const std::string command = "announce_done:ui:" + event_id;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!running_.load() || stop_.load()) return HandshakeOutcome::failed;

        std::string resp;
        std::string err;
        if (send_command(capture_sock_, command, kConnectTimeoutMs,
                         kIoTimeoutMs, &resp, &err)) {
            if (resp.find("\"duplicate\":true") != std::string::npos) {
                return HandshakeOutcome::success;
            }
            if (resp.find("\"ok\":true") != std::string::npos) {
                return HandshakeOutcome::success;
            }
            if (resp.find("\"error\":\"invalid_phase\"") != std::string::npos) {
                return HandshakeOutcome::invalid_phase;
            }
            err = "unexpected response: " + resp;
        }
        log_warn("phase watcher: announce_done attempt %d failed for %s: %s",
                 attempt + 1, event_id.c_str(), err.c_str());
        if (attempt < 3) {
            if (!sleep_ms(kRetryDelaysMs[attempt])) {
                return HandshakeOutcome::failed;
            }
        }
    }
    return HandshakeOutcome::failed;
}

void PhaseWatcher::maybe_handshake(int phase_version,
                                   const PhaseStatus& status) {
    bool needed = false;
    {
        std::lock_guard<std::mutex> lock(policy_mu_);
        needed = policy_.handshake_unconfirmed(phase_version);
    }
    if (!needed) {
        std::lock_guard<std::mutex> lock(mail_mu_);
        if (handshake_requested_version_ == phase_version) {
            handshake_requested_version_ = -1;
        }
        return;
    }
    if (!running_.load() || stop_.load()) return;

    if (handshake_version_ != phase_version || handshake_event_id_.empty()) {
        handshake_version_ = phase_version;
        handshake_event_id_ = make_event_id();
    }
    const HandshakeOutcome outcome = send_handshake(handshake_event_id_);
    {
        std::lock_guard<std::mutex> lock(policy_mu_);
        policy_.on_handshake_result(phase_version, outcome);
    }
    log_voice(status, "announce_done", handshake_outcome_name(outcome),
              "announce_done event=" + handshake_event_id_);

    // One attempt per request; polling re-requests while still pending.
    std::lock_guard<std::mutex> lock(mail_mu_);
    if (handshake_requested_version_ == phase_version) {
        handshake_requested_version_ = -1;
    }
}

void PhaseWatcher::voice_loop() {
    std::unique_lock<std::mutex> lock(mail_mu_);
    while (!worker_stop_) {
        // A pending handshake is I/O, not audio: send it before queued
        // feedback so a button voice can never delay the state machine. It is
        // held back while a prompt is still queued (the prompt owns its own
        // post-playback handshake).
        if (handshake_requested_version_ >= 0 && !prompt_job_) {
            const int version = handshake_requested_version_;
            const PhaseStatus status = handshake_requested_status_;
            lock.unlock();
            maybe_handshake(version, status);
            lock.lock();
            continue;
        }
        // Voice jobs: button > prompt > complete.
        if (button_job_) {
            const VoiceJob job = std::move(*button_job_);
            button_job_.reset();
            prompt_superseded_ = false;  // the key is being played now
            lock.unlock();
            play_button(job);
            lock.lock();
            continue;
        }
        if (prompt_job_) {
            const VoiceJob job = std::move(*prompt_job_);
            prompt_job_.reset();
            prompt_superseded_ = false;  // a later key sets it again
            lock.unlock();
            play_flow_voice(job, "zero_done_speaking prompt", kPromptQueueWaitMs,
                            true);
            // The prompt was spoken (or preempted/skipped); finish its
            // handshake before the next voice.
            maybe_handshake(job.status.phase_version, job.status);
            lock.lock();
            continue;
        }
        if (complete_job_) {
            const VoiceJob job = std::move(*complete_job_);
            complete_job_.reset();
            lock.unlock();
            play_flow_voice(job, "calibration_complete prompt",
                            kPromptQueueWaitMs, false);
            lock.lock();
            continue;
        }
        mail_cv_.wait(lock);
    }
}

// ---------------------------------------------------------------------------
// Polling loop
// ---------------------------------------------------------------------------
void PhaseWatcher::run() {
    log_info("phase watcher: start (socket=%s)", capture_sock_.c_str());
    while (running_.load() && !stop_.load()) {
        std::string resp;
        std::string err;
        PhaseStatus status;
        bool ok = send_command(capture_sock_, "phase_status", kConnectTimeoutMs,
                               kIoTimeoutMs, &resp, &err);
        if (ok && !parse_phase_status(resp, status)) {
            ok = false;
            err = "invalid phase_status response";
        }

        int wait_ms = 0;
        if (ok) {
            if (failing_) {
                log_info("phase watcher: recovered");
                failing_ = false;
                failure_count_ = 0;
                backoff_index_ = 0;
            }
            handle_status(status);
            wait_ms = phase_voice::poll_interval_ms(status.phase);
        } else {
            note_failure(err);
            static const int kBackoffMs[4] = {300, 1000, 2000, 5000};
            wait_ms = kBackoffMs[backoff_index_ < 3 ? backoff_index_ : 3];
            if (backoff_index_ < 3) ++backoff_index_;
        }

        if (!sleep_ms(wait_ms)) break;
    }
    log_info("phase watcher: exit");
}
