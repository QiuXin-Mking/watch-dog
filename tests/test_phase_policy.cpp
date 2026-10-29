// Host tests for the pure PhaseVoicePolicy mapping / dedupe / reset logic.

#include <cstdio>
#include <string>

#include "phase_voice_policy.h"

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

PhaseStatus status(const std::string& phase, int version,
                   const std::string& event = "", const std::string& source = "",
                   const std::string& action = "",
                   const std::string& result = "",
                   const std::string& task = "") {
    PhaseStatus st;
    st.valid = true;
    st.phase = phase;
    st.phase_version = version;
    st.last_event_id = event;
    st.last_source = source;
    st.last_action = action;
    st.last_result = result;
    st.task_id = task;
    return st;
}

}  // namespace

int main() {
    // ---- button mapping table ----
    CHECK(phase_voice::button_wav_for("zero_start", "ok") ==
          phase_voice::kZeroStartWav);
    CHECK(phase_voice::button_wav_for("zero_cancel", "ok") ==
          phase_voice::kZeroCancelWav);
    CHECK(phase_voice::button_wav_for("prep_start", "ok") ==
          phase_voice::kPrepStartWav);
    CHECK(phase_voice::button_wav_for("prep_start", "invalid_phase") ==
          phase_voice::kPrepRejectedWav);
    CHECK(phase_voice::button_wav_for("prep_finish", "prep_below_threshold") ==
          phase_voice::kPrepRejectedWav);
    CHECK(phase_voice::button_wav_for("zero_start", "invalid_phase") ==
          phase_voice::kCannotStartWav);
    CHECK(phase_voice::button_wav_for("zero_start", "not_claimed") ==
          phase_voice::kClaimTaskWav);
    CHECK(phase_voice::button_wav_for("prep_start", "not_claimed") ==
          phase_voice::kClaimTaskWav);
    // Not mapped: prep_finish+ok, zero_cancel+invalid_phase, busy, unknown.
    CHECK(phase_voice::button_wav_for("prep_finish", "ok").empty());
    CHECK(phase_voice::button_wav_for("zero_cancel", "invalid_phase").empty());
    CHECK(phase_voice::button_wav_for("zero_start", "busy").empty());
    CHECK(phase_voice::button_wav_for("what", "ok").empty());

    // ---- poll cadence ----
    CHECK(phase_voice::poll_interval_ms("preview_ready") == 500);
    CHECK(phase_voice::poll_interval_ms("calibration_complete") == 500);
    CHECK(phase_voice::poll_interval_ms("zero_wait") == 150);
    CHECK(phase_voice::poll_interval_ms("zero_done_speaking") == 150);
    CHECK(phase_voice::poll_interval_ms("prep_recording") == 150);

    // ---- baseline: no historical button replay ----
    {
        PhaseVoicePolicy p;
        auto d = p.observe(status("preview_ready", 0, "old", "gpio", "zero_start",
                                  "ok"));
        CHECK(d.button_wav.empty());
        // Same event observed again: still silent.
        d = p.observe(status("preview_ready", 0, "old", "gpio", "zero_start", "ok"));
        CHECK(d.button_wav.empty());
        // New gpio event plays once, then dedupes for 10 polls.
        d = p.observe(status("preview_ready", 1, "e1", "gpio", "zero_start", "ok"));
        CHECK(d.button_wav == phase_voice::kZeroStartWav);
        for (int i = 0; i < 10; ++i) {
            d = p.observe(
                status("preview_ready", 1, "e1", "gpio", "zero_start", "ok"));
            CHECK(d.button_wav.empty());
        }
        // ui source never plays but advances the dedupe key.
        d = p.observe(status("preview_ready", 2, "e2", "ui", "zero_start", "ok"));
        CHECK(d.button_wav.empty());
        // Event id cleared (release), then a fresh event fires again.
        d = p.observe(status("preview_ready", 3, "", "", "", ""));
        CHECK(d.button_wav.empty());
        d = p.observe(status("preview_ready", 4, "e3", "gpio", "zero_cancel", "ok"));
        CHECK(d.button_wav == phase_voice::kZeroCancelWav);
    }

    // ---- zero_done_speaking prompt + handshake dedupe ----
    {
        PhaseVoicePolicy p;
        PhaseStatus st = status("zero_done_speaking", 5);
        auto d = p.observe(st);
        CHECK(d.zero_prompt_wav == phase_voice::kZeroPromptWav);
        CHECK(d.handshake_pending);
        for (int i = 0; i < 10; ++i) {
            d = p.observe(st);
            CHECK(d.zero_prompt_wav.empty());
            CHECK(d.handshake_pending);  // still unconfirmed
        }
        p.on_handshake_result(5, HandshakeOutcome::success);
        d = p.observe(st);
        CHECK(d.zero_prompt_wav.empty());
        CHECK(!d.handshake_pending);
    }

    // ---- invalid_phase clears the pending handshake too ----
    {
        PhaseVoicePolicy p;
        PhaseStatus st = status("zero_done_speaking", 9);
        auto d = p.observe(st);
        CHECK(d.handshake_pending);
        p.on_handshake_result(9, HandshakeOutcome::invalid_phase);
        d = p.observe(st);
        CHECK(!d.handshake_pending);
    }

    // ---- a button in the same observation wins over the prompt ----
    {
        PhaseVoicePolicy p;
        p.observe(status("preview_ready", 0));
        auto d = p.observe(status("zero_done_speaking", 1, "k1", "gpio",
                                  "prep_start", "invalid_phase"));
        CHECK(d.button_wav == phase_voice::kPrepRejectedWav);
        CHECK(d.zero_prompt_wav.empty());
        CHECK(d.handshake_pending);
    }

    // ---- calibration_complete: no replay at startup, play on transition ----
    {
        PhaseVoicePolicy p;
        auto d = p.observe(status("calibration_complete", 3));
        CHECK(d.complete_wav.empty());
        d = p.observe(status("calibration_complete", 3));
        CHECK(d.complete_wav.empty());
    }
    {
        PhaseVoicePolicy p;
        p.observe(status("prep_done", 0));
        auto d = p.observe(status("calibration_complete", 1));
        CHECK(d.complete_wav == phase_voice::kCompleteWav);
        d = p.observe(status("calibration_complete", 1));
        CHECK(d.complete_wav.empty());
    }

    // ---- backend restart: dedupe reset, no old button/complete replay ----
    {
        PhaseVoicePolicy p;
        p.observe(status("preview_ready", 10, "old", "gpio", "zero_start", "ok"));
        auto d = p.observe(status("calibration_complete", 0, "old", "gpio",
                                  "zero_start", "ok", ""));
        CHECK(d.complete_wav.empty());  // new instance, no replay
        CHECK(d.button_wav.empty());
        d = p.observe(status("preview_ready", 1, "n1", "gpio", "zero_start", "ok"));
        CHECK(d.button_wav == phase_voice::kZeroStartWav);
    }
    {
        // Restart into zero_done_speaking re-prompts and handshakes.
        PhaseVoicePolicy p;
        p.observe(status("preview_ready", 10));
        auto d = p.observe(status("zero_done_speaking", 0));
        CHECK(d.zero_prompt_wav == phase_voice::kZeroPromptWav);
        CHECK(d.handshake_pending);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_phase_policy: ok\n");
    return 0;
}
