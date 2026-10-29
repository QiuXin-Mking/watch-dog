#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "spec.h"
#include "wav.h"

// Preloads every WAV referenced by the camera specs from <base_dir>. A missing
// file is logged once and that channel degrades to journald-only (no crash).
class SoundBank {
public:
    void add_from_specs(const std::vector<CameraSpec>& specs,
                        const std::string& base_dir);
    // Load one WAV by file name (used for non-camera sensor channels). Missing
    // / unusable files are logged once and skipped, never fatal.
    void add(const std::string& file_name, const std::string& base_dir);
    const WavData* find(const std::string& file_name) const;
    size_t count() const { return wavs_.size(); }

private:
    void load(const std::string& file_name, const std::string& base_dir);

    std::map<std::string, std::unique_ptr<WavData>> wavs_;
    std::vector<std::string> warned_;  // already-logged missing files
};

// Audio priority: lower value wins. Shutdown > alarm > button > flow prompt.
enum class VoicePriority {
    shutdown = 0,
    alarm = 1,
    button = 2,
    prompt = 3,
};

enum class PlayResult {
    played,     // whole WAV written
    cancelled,  // caller's cancel flag tripped
    preempted,  // a higher/equal-priority request asked us to stop
    skipped,    // arbiter declined (busy / superseded) -- never returned by sink
    error,      // invalid WAV or ALSA failure
};

// The playback primitive behind PlaybackArbiter. Implementations must check
// `cancel` and (when non-null) `preempt` between write chunks.
class VoiceSink {
public:
    virtual ~VoiceSink() = default;
    virtual PlayResult play(const WavData& wav, const std::atomic<bool>& cancel,
                            const std::atomic<bool>* preempt) = 0;
};

// Streams raw PCM (S16_LE) to an ALSA PCM device ("plughw:..." allows format /
// rate / channel conversion). Each play() opens, writes in ~20 ms chunks and
// closes; cancel and preempt are checked between chunks so stop_alarm / a
// higher-priority voice mutes fast.
class AudioPlayer : public VoiceSink {
public:
    explicit AudioPlayer(std::string device) : device_(std::move(device)) {}

    void set_device(std::string d) { device_ = std::move(d); }
    const std::string& device() const { return device_; }

    PlayResult play(const WavData& wav, const std::atomic<bool>& cancel,
                    const std::atomic<bool>* preempt) override;

    // Legacy convenience wrapper (alarm semantics: no preemption source).
    bool play(const WavData& wav, const std::atomic<bool>& cancel);

private:
    std::string device_;
};
