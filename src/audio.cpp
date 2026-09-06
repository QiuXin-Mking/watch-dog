#include "audio.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "log.h"

// ---------------------------------------------------------------------------
// SoundBank
// ---------------------------------------------------------------------------
void SoundBank::add_from_specs(const std::vector<CameraSpec>& specs,
                               const std::string& base_dir) {
    const auto load = [&](const std::string& file_name, bool alarm) {
        if (file_name.empty()) return;
        if (wavs_.count(file_name)) return;
        const std::string path = base_dir + "/" + file_name;
        auto wav = std::make_unique<WavData>();
        std::string err;
        if (!load_wav_file(path, *wav, err)) {
            log_warn("sound: %s (missing/unusable WAV -> %s journald-only)",
                     err.c_str(), alarm ? "alarm degrades" : "recover degrades");
            return;
        }
        log_info("sound: loaded %s (%u Hz, %u ch, %zu bytes)", path.c_str(),
                 wav->sample_rate, wav->channels, wav->pcm.size());
        wavs_[file_name] = std::move(wav);
    };

    for (const auto& s : specs) {
        load(s.alarm_wav, true);
        load(s.recover_wav, false);
    }
}

const WavData* SoundBank::find(const std::string& file_name) const {
    auto it = wavs_.find(file_name);
    return it == wavs_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// AudioPlayer
// ---------------------------------------------------------------------------
bool AudioPlayer::play(const WavData& wav, const std::atomic<bool>& cancel) {
    if (!wav.valid()) return false;
    if (cancel.load()) return false;

    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        log_error("audio: snd_pcm_open(%s) failed: %s", device_.c_str(),
                  snd_strerror(rc));
        return false;
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
        return false;
    }
    rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        log_error("audio: set_access failed: %s", snd_strerror(rc));
        finish(false);
        return false;
    }
    rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {
        log_error("audio: set_format failed: %s", snd_strerror(rc));
        finish(false);
        return false;
    }
    rc = snd_pcm_hw_params_set_channels(pcm, hw, wav.channels);
    if (rc < 0) {
        log_error("audio: set_channels(%u) failed: %s", wav.channels,
                  snd_strerror(rc));
        finish(false);
        return false;
    }
    unsigned int rate = wav.sample_rate;
    rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    if (rc < 0) {
        log_error("audio: set_rate_near(%u) failed: %s", wav.sample_rate,
                  snd_strerror(rc));
        finish(false);
        return false;
    }
    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        log_error("audio: hw_params failed: %s", snd_strerror(rc));
        finish(false);
        return false;
    }

    // Start as soon as the first frames are written (short beeps stay audible).
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    rc = snd_pcm_sw_params_current(pcm, sw);
    if (rc < 0) {
        log_error("audio: sw_params_current failed: %s", snd_strerror(rc));
        finish(false);
        return false;
    }
    snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    snd_pcm_sw_params_set_avail_min(pcm, sw, 1);
    rc = snd_pcm_sw_params(pcm, sw);
    if (rc < 0) {
        log_error("audio: sw_params failed: %s", snd_strerror(rc));
        finish(false);
        return false;
    }

    const uint32_t frame_size = wav.channels * 2u;  // S16_LE
    const size_t total_frames = wav.pcm.size() / frame_size;
    const snd_pcm_uframes_t chunk_frames =
        std::max<snd_pcm_uframes_t>(128, wav.sample_rate / 50);  // ~20 ms

    bool ok = false;
    size_t frame_offset = 0;
    while (frame_offset < total_frames) {
        if (cancel.load()) break;  // stop_alarm / new event -> mute quickly

        const snd_pcm_uframes_t n = static_cast<snd_pcm_uframes_t>(
            std::min<size_t>(chunk_frames, total_frames - frame_offset));
        const uint8_t* ptr =
            wav.pcm.data() + frame_offset * frame_size;

        snd_pcm_sframes_t written = snd_pcm_writei(pcm, ptr, n);
        if (written < 0) {
            if (written == -EPIPE) {
                // Buffer underrun: re-prepare and continue from where we were.
                if (snd_pcm_prepare(pcm) < 0) break;
                continue;
            }
            if (written == -EAGAIN) continue;
            log_error("audio: snd_pcm_writei failed: %s", snd_strerror(static_cast<int>(written)));
            break;
        }
        frame_offset += static_cast<size_t>(written);
    }

    ok = (frame_offset >= total_frames && !cancel.load());
    finish(ok);
    return ok;
}
