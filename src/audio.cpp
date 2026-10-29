#include "audio.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "log.h"

bool AudioPlayer::play(const WavData& wav, const std::atomic<bool>& cancel) {
    return play(wav, cancel, nullptr) == PlayResult::played;
}

PlayResult AudioPlayer::play(const WavData& wav,
                             const std::atomic<bool>& cancel,
                             const std::atomic<bool>* preempt) {
    if (!wav.valid()) return PlayResult::error;
    if (cancel.load()) return PlayResult::cancelled;
    if (preempt && preempt->load()) return PlayResult::preempted;

    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        log_error("audio: snd_pcm_open(%s) failed: %s", device_.c_str(),
                  snd_strerror(rc));
        return PlayResult::error;
    }

    const auto finish = [&](bool ok) {
        if (ok) {
            snd_pcm_drain(pcm);
        } else {
            snd_pcm_drop(pcm);
        }
        snd_pcm_close(pcm);
    };

    // Hardware params (plughw converts format/rate/channels as needed).
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    rc = snd_pcm_hw_params_any(pcm, hw);
    if (rc < 0) {
        log_error("audio: hw_params_any failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        log_error("audio: set_access failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {
        log_error("audio: set_format failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    rc = snd_pcm_hw_params_set_channels(pcm, hw, wav.channels);
    if (rc < 0) {
        log_error("audio: set_channels(%u) failed: %s", wav.channels,
                  snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    unsigned int rate = wav.sample_rate;
    rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    if (rc < 0) {
        log_error("audio: set_rate_near(%u) failed: %s", wav.sample_rate,
                  snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        log_error("audio: hw_params failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }

    // Start as soon as the first frames are written (short beeps stay audible).
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    rc = snd_pcm_sw_params_current(pcm, sw);
    if (rc < 0) {
        log_error("audio: sw_params_current failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }
    snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    snd_pcm_sw_params_set_avail_min(pcm, sw, 1);
    rc = snd_pcm_sw_params(pcm, sw);
    if (rc < 0) {
        log_error("audio: sw_params failed: %s", snd_strerror(rc));
        finish(false);
        return PlayResult::error;
    }

    const uint32_t frame_size = wav.channels * 2u;  // S16_LE
    const size_t total_frames = wav.pcm.size() / frame_size;
    const snd_pcm_uframes_t chunk_frames =
        std::max<snd_pcm_uframes_t>(128, wav.sample_rate / 50);  // ~20 ms

    PlayResult result = PlayResult::played;
    size_t frame_offset = 0;
    while (frame_offset < total_frames) {
        if (cancel.load()) {
            result = PlayResult::cancelled;  // stop_alarm / shutdown
            break;
        }
        if (preempt && preempt->load()) {
            result = PlayResult::preempted;  // higher-priority voice
            break;
        }

        const snd_pcm_uframes_t n = static_cast<snd_pcm_uframes_t>(
            std::min<size_t>(chunk_frames, total_frames - frame_offset));
        const uint8_t* ptr =
            wav.pcm.data() + frame_offset * frame_size;

        snd_pcm_sframes_t written = snd_pcm_writei(pcm, ptr, n);
        if (written < 0) {
            if (written == -EPIPE) {
                // Buffer underrun: re-prepare and continue from where we were.
                if (snd_pcm_prepare(pcm) < 0) {
                    result = PlayResult::error;
                    break;
                }
                continue;
            }
            if (written == -EAGAIN) continue;
            log_error("audio: snd_pcm_writei failed: %s", snd_strerror(static_cast<int>(written)));
            result = PlayResult::error;
            break;
        }
        frame_offset += static_cast<size_t>(written);
    }

    if (result == PlayResult::played) {
        if (cancel.load()) {
            result = PlayResult::cancelled;
        } else if (preempt && preempt->load()) {
            result = PlayResult::preempted;
        } else if (frame_offset < total_frames) {
            result = PlayResult::error;
        }
    }
    finish(result == PlayResult::played);
    return result;
}
