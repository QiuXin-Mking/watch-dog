#include "phase_voice_policy.h"

namespace phase_voice {

std::vector<std::string> all_calibration_wavs() {
    return {
        kZeroPromptWav, kZeroStartWav,    kZeroCancelWav, kPrepStartWav,
        kCompleteWav,   kCannotStartWav,  kPrepRejectedWav, kClaimTaskWav,
        kBusyWav,
    };
}

std::string button_wav_for(const std::string& action, const std::string& result) {
    if (action == "zero_start") {
        if (result == "ok") return kZeroStartWav;
        if (result == "invalid_phase") return kCannotStartWav;
        if (result == "not_claimed") return kClaimTaskWav;
        return "";
    }
    if (action == "zero_cancel") {
        // invalid_phase (e.g. already in zero_done_speaking) is intentionally
        // silent; the completion prompt covers that transition.
        if (result == "ok") return kZeroCancelWav;
        return "";
    }
    if (action == "prep_start") {
        if (result == "ok") return kPrepStartWav;
        if (result == "invalid_phase") return kPrepRejectedWav;
        if (result == "not_claimed") return kClaimTaskWav;
        return "";
    }
    if (action == "prep_finish") {
        // prep_finish + ok is covered by the following calibration_complete.
        if (result == "prep_below_threshold") return kPrepRejectedWav;
        return "";
    }
    return "";
}

bool is_active_phase(const std::string& phase) {
    return phase.rfind("zero_", 0) == 0 || phase.rfind("prep_", 0) == 0;
}

int poll_interval_ms(const std::string& phase) {
    return is_active_phase(phase) ? 150 : 500;
}

}  // namespace phase_voice

bool PhaseVoicePolicy::backend_restarted(const PhaseStatus& status) const {
    if (!baseline_done_) return false;
    if (status.phase_version < last_phase_version_) return true;
    // Version reset to zero together with a cleared task is also a new daemon.
    return status.phase_version == 0 && status.task_id.empty() &&
           last_phase_version_ > 0;
}

void PhaseVoicePolicy::reset() {
    baseline_done_ = false;
    last_event_id_.clear();
    last_phase_.clear();
    last_phase_version_ = -1;
    zero_prompt_version_ = -1;
    zero_handshake_version_ = -1;
    complete_version_ = -1;
}

VoiceDecision PhaseVoicePolicy::observe(const PhaseStatus& status) {
    VoiceDecision decision;
    if (!status.valid) return decision;

    if (backend_restarted(status)) {
        // New daemon instance: drop all dedupe memory, then treat this
        // observation as a fresh baseline (never replay old buttons/complete).
        reset();
    }

    if (!baseline_done_) {
        baseline_done_ = true;
        last_event_id_ = status.last_event_id;
        last_phase_ = status.phase;
        last_phase_version_ = status.phase_version;
        if (status.phase == "zero_done_speaking") {
            // Recovery path: watchdog (re)started while the daemon waits for
            // our voice + handshake.
            zero_prompt_version_ = status.phase_version;
            decision.zero_prompt_wav = phase_voice::kZeroPromptWav;
            decision.handshake_pending =
                status.phase_version != zero_handshake_version_;
        } else if (status.phase == "calibration_complete") {
            // Do not replay on startup.
            complete_version_ = status.phase_version;
        }
        return decision;
    }

    // Physical-key feedback: dedupe by event id, gpio only.
    if (status.last_event_id != last_event_id_) {
        last_event_id_ = status.last_event_id;
        if (status.last_source == "gpio" && !status.last_event_id.empty()) {
            decision.button_wav =
                phase_voice::button_wav_for(status.last_action, status.last_result);
        }
    }

    if (status.phase == "zero_done_speaking") {
        if (status.phase_version != zero_prompt_version_) {
            zero_prompt_version_ = status.phase_version;
            // A button event in the same observation wins over the long prompt
            // (typical when the watcher just started); handshake still follows.
            if (decision.button_wav.empty() &&
                status.phase_version != zero_handshake_version_) {
                decision.zero_prompt_wav = phase_voice::kZeroPromptWav;
            }
        }
        if (status.phase_version != zero_handshake_version_) {
            decision.handshake_pending = true;
        }
    } else {
        // Left zero_done_speaking: forget the prompt attempt.
        zero_prompt_version_ = -1;
    }

    if (status.phase == "calibration_complete") {
        const bool entered = last_phase_ != "calibration_complete";
        if (entered && complete_version_ != status.phase_version) {
            decision.complete_wav = phase_voice::kCompleteWav;
        }
        complete_version_ = status.phase_version;
    }

    last_phase_ = status.phase;
    last_phase_version_ = status.phase_version;
    return decision;
}

void PhaseVoicePolicy::on_handshake_result(int phase_version,
                                           HandshakeOutcome outcome) {
    if (outcome == HandshakeOutcome::success ||
        outcome == HandshakeOutcome::invalid_phase) {
        // invalid_phase means someone else advanced the phase; stop retrying.
        zero_handshake_version_ = phase_version;
    }
}

bool PhaseVoicePolicy::handshake_unconfirmed(int phase_version) const {
    return phase_version >= 0 && zero_handshake_version_ != phase_version;
}
