#pragma once

#include <atomic>
#include <mutex>

#include "audio.h"

// Serializes access to a single VoiceSink and applies the F-05 priority rules:
// shutdown > alarm > button > prompt. A higher-or-equal priority request sets
// the sink's preempt flag (the sink stops within one write chunk); a lower one
// waits up to max_wait_ms and is skipped when the speaker stays busy.
class PlaybackArbiter {
public:
    explicit PlaybackArbiter(VoiceSink& sink) : sink_(sink) {}
    ~PlaybackArbiter() = default;

    PlaybackArbiter(const PlaybackArbiter&) = delete;
    PlaybackArbiter& operator=(const PlaybackArbiter&) = delete;

    PlayResult play(const WavData& wav, const std::atomic<bool>& cancel,
                    VoicePriority prio, int max_wait_ms = 0);

    // Asks the currently playing voice to stop when `prio` should win. Also
    // records the request so a lower-priority `play()` that is still waiting
    // for the lock yields instead of starting (the speaker may be busy with an
    // alarm, where an in-flight preempt flag cannot be set yet).
    void request_preempt(VoicePriority prio);

    VoicePriority current_priority() const;

private:
    VoiceSink& sink_;
    mutable std::mutex state_mu_;
    std::timed_mutex playback_mu_;
    bool active_ = false;
    VoicePriority current_prio_ = VoicePriority::prompt;
    VoicePriority latest_prio_ = VoicePriority::prompt;
    unsigned long latest_gen_ = 0;
    // Logical clock plus the last sequence at which each priority was
    // requested via request_preempt(). A play() captures the clock on entry and
    // gives up if a strictly higher-priority request arrived afterwards.
    static constexpr int kPriorityCount =
        static_cast<int>(VoicePriority::prompt) + 1;
    unsigned long request_seq_ = 0;
    unsigned long preempt_seq_[kPriorityCount] = {};
    std::atomic<bool> preempt_{false};
};
