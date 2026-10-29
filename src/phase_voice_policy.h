#pragma once

#include <string>
#include <vector>

#include "phase_state_view.h"

// F-05 voice asset names (must match sounds/ verbatim).
namespace phase_voice {

inline constexpr const char* kZeroPromptWav =
    "标0完成，请穿戴设备，录制准备动作.wav";
inline constexpr const char* kZeroStartWav = "标定已开始，请保持静止.wav";
inline constexpr const char* kZeroCancelWav = "标定已取消.wav";
inline constexpr const char* kPrepStartWav = "准备动作录制中.wav";
inline constexpr const char* kCompleteWav = "标定完成.wav";
inline constexpr const char* kCannotStartWav = "当前不能开始标定.wav";
inline constexpr const char* kPrepRejectedWav = "未按要求执行准备动作录制.wav";
inline constexpr const char* kClaimTaskWav = "请先选择任务.wav";
// Reserved: `busy` rejections are not written to phase_status.last_* by the
// backend, so this voice is never selected in this phase of work.
inline constexpr const char* kBusyWav = "设备正在收尾，请稍后.wav";

// All calibration WAVs the process preloads.
std::vector<std::string> all_calibration_wavs();

// Pure mapping for a physical-key event; empty string = do not play.
std::string button_wav_for(const std::string& action, const std::string& result);

// Poll cadence: active calibration phases poll faster.
bool is_active_phase(const std::string& phase);
int poll_interval_ms(const std::string& phase);

}  // namespace phase_voice

// What the watcher should do for one phase_status observation.
struct VoiceDecision {
    std::string button_wav;       // empty = no button feedback
    std::string zero_prompt_wav;  // empty = no zero_done_speaking prompt
    std::string complete_wav;     // empty = no calibration_complete voice
    bool handshake_pending = false;  // announce_done still needs to be sent
};

enum class HandshakeOutcome { success, invalid_phase, failed };

// Pure dedupe/transition state machine. No sockets, no threading, no ALSA.
class PhaseVoicePolicy {
public:
    VoiceDecision observe(const PhaseStatus& status);
    void on_handshake_result(int phase_version, HandshakeOutcome outcome);
    // True while the prompt for `phase_version` has not been confirmed. Kept
    // independent of the latest phase so a prompt interrupted by a key event
    // still completes its announce_done handshake.
    bool handshake_unconfirmed(int phase_version) const;
    void reset();

private:
    bool backend_restarted(const PhaseStatus& status) const;

    bool baseline_done_ = false;
    std::string last_event_id_;
    std::string last_phase_;
    int last_phase_version_ = -1;
    int zero_prompt_version_ = -1;
    int zero_handshake_version_ = -1;
    int complete_version_ = -1;
};
