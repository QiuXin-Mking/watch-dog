#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Raw PCM WAV payload kept in memory (16-bit signed little-endian).
struct WavData {
    std::vector<uint8_t> pcm;  // raw interleaved PCM bytes (S16_LE)
    uint32_t sample_rate = 0;
    uint16_t channels = 0;  // expected 1 (mono)
    std::string source;     // file path, for log messages

    bool valid() const {
        return sample_rate != 0 && channels != 0 && !pcm.empty();
    }
};

// Parse a RIFF/WAVE container. Only uncompressed PCM (fmt tag 1) 16-bit is
// accepted. On failure returns false and fills err with a human-readable reason.
bool load_wav_file(const std::string& path, WavData& out, std::string& err);
