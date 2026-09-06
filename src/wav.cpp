#include "wav.h"

#include <cstdio>
#include <cstring>

namespace {

uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

bool has_prefix(const uint8_t* p, const char* s) {
    return std::memcmp(p, s, std::strlen(s)) == 0;
}

}  // namespace

bool load_wav_file(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "open failed: " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size_l = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size_l < 12) {
        std::fclose(f);
        err = "too small to be RIFF: " + path;
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size_l));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        err = "read failed: " + path;
        return false;
    }
    std::fclose(f);

    if (!has_prefix(buf.data(), "RIFF") || !has_prefix(buf.data() + 8, "WAVE")) {
        err = "not a RIFF/WAVE file: " + path;
        return false;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    bool have_fmt = false;
    bool have_data = false;

    size_t pos = 12;
    const size_t end = buf.size();
    while (pos + 8 <= end) {
        const uint8_t* id = buf.data() + pos;
        uint32_t chunk_size = rd_u32(buf.data() + pos + 4);
        const size_t body = pos + 8;
        if (chunk_size > end - body) chunk_size = static_cast<uint32_t>(end - body);

        if (has_prefix(id, "fmt ") && chunk_size >= 16) {
            audio_format = rd_u16(buf.data() + body);
            channels = rd_u16(buf.data() + body + 2);
            sample_rate = rd_u32(buf.data() + body + 4);
            bits = rd_u16(buf.data() + body + 14);
            have_fmt = true;
        } else if (has_prefix(id, "data")) {
            out.pcm.assign(buf.data() + body, buf.data() + body + chunk_size);
            have_data = true;
        }

        pos = body + chunk_size + (chunk_size & 1u);  // chunks are word-aligned
    }

    if (!have_fmt || !have_data) {
        err = "missing fmt/data chunk: " + path;
        return false;
    }
    if (audio_format != 1) {
        err = "not uncompressed PCM (format=" + std::to_string(audio_format) + "): " + path;
        return false;
    }
    if (bits != 16) {
        err = "not 16-bit PCM (bits=" + std::to_string(bits) + "): " + path;
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        err = "bad channels/rate: " + path;
        return false;
    }
    out.sample_rate = sample_rate;
    out.channels = channels;
    out.source = path;
    return true;
}
