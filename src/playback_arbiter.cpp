#include "playback_arbiter.h"

#include <chrono>

PlayResult PlaybackArbiter::play(const WavData& wav,
                                 const std::atomic<bool>& cancel,
                                 VoicePriority prio, int max_wait_ms) {
    unsigned long my_gen = 0;
    unsigned long my_request_seq = 0;
    bool should_preempt = false;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        my_gen = ++latest_gen_;
        latest_prio_ = prio;
        my_request_seq = request_seq_;
        should_preempt = prio <= current_prio_;
        if (should_preempt) preempt_.store(true);
    }

    // A higher/equal request blocks until the current voice yields; a lower one
    // gives up after max_wait_ms so a long alarm is never queued behind.
    bool locked = false;
    if (should_preempt) {
        playback_mu_.lock();
        locked = true;
    } else if (max_wait_ms > 0) {
        locked = playback_mu_.try_lock_for(std::chrono::milliseconds(max_wait_ms));
    } else {
        locked = playback_mu_.try_lock();
    }
    if (!locked) return PlayResult::skipped;

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        // A newer request of the same or higher priority already superseded us
        // (e.g. two rapid button presses): let the newest play instead.
        bool superseded = my_gen != latest_gen_ && latest_prio_ <= prio;
        // A higher-priority voice was requested (from the polling thread) while
        // we were waiting for the lock: it must not start behind us.
        for (int p = 0; !superseded && p < static_cast<int>(prio); ++p) {
            superseded = preempt_seq_[p] > my_request_seq;
        }
        if (superseded) {
            playback_mu_.unlock();
            return PlayResult::skipped;
        }
        current_prio_ = prio;
        active_ = true;
        // Clear preemption raised by our own request; a newer request arriving
        // after this point will set it again under state_mu_.
        preempt_.store(false);
    }

    const PlayResult result = sink_.play(wav, cancel, &preempt_);

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        active_ = false;
        current_prio_ = prio;
    }
    playback_mu_.unlock();
    return result;
}

void PlaybackArbiter::request_preempt(VoicePriority prio) {
    std::lock_guard<std::mutex> lock(state_mu_);
    preempt_seq_[static_cast<int>(prio)] = ++request_seq_;
    if (active_ && prio <= current_prio_) {
        preempt_.store(true);
    }
}

VoicePriority PlaybackArbiter::current_priority() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return current_prio_;
}
