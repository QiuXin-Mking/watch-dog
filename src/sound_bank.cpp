#include "audio.h"

#include <memory>
#include <utility>

#include "log.h"

// Split from audio.cpp so host tests can link the WAV cache without pulling in
// <alsa/asoundlib.h>.

void SoundBank::load(const std::string& file_name, const std::string& base_dir) {
    if (file_name.empty()) return;
    if (wavs_.count(file_name)) return;
    const std::string path = base_dir + "/" + file_name;
    auto wav = std::make_unique<WavData>();
    std::string err;
    if (!load_wav_file(path, *wav, err)) {
        log_warn("sound: %s (missing/unusable WAV -> journald-only)",
                 err.c_str());
        return;
    }
    // ES8323/I2S 播放链路要求双声道 frame slot：单声道数据会被按双声道 slot
    // 消耗，导致约 1.9 倍速、音调尖细（根因见 docs/01-声音尖细根因.MD）。
    // 加载时把 mono 复制成双声道（L=R），播放端统一按 2 声道写，不再依赖
    // 「语音文件必须本身是 stereo」的约定。
    if (wav->channels == 1) {
        const size_t n = wav->pcm.size() / 2u;  // int16 采样点个数
        std::vector<uint8_t> stereo;
        stereo.reserve(n * 4u);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t lo = wav->pcm[i * 2];
            const uint8_t hi = wav->pcm[i * 2 + 1];
            stereo.push_back(lo);
            stereo.push_back(hi);
            stereo.push_back(lo);  // 左声道 = 右声道
            stereo.push_back(hi);
        }
        wav->pcm = std::move(stereo);
        wav->channels = 2;
    }
    log_info("sound: loaded %s (%u Hz, %u ch, %zu bytes)", path.c_str(),
             wav->sample_rate, wav->channels, wav->pcm.size());
    wavs_[file_name] = std::move(wav);
}

void SoundBank::add_from_specs(const std::vector<CameraSpec>& specs,
                               const std::string& base_dir) {
    for (const auto& s : specs) {
        load(s.alarm_wav, base_dir);
        load(s.recover_wav, base_dir);
    }
}

void SoundBank::add(const std::string& file_name, const std::string& base_dir) {
    load(file_name, base_dir);
}

const WavData* SoundBank::find(const std::string& file_name) const {
    auto it = wavs_.find(file_name);
    return it == wavs_.end() ? nullptr : it->second.get();
}
