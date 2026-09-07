#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "spec.h"

// Shared, mutex-protected runtime state.
//
// A "channel" is the logical alarm unit named by CameraSpec::name. One channel
// may be backed by several device specs (e.g. "sixcam" = jhh02 2d50 + jhh04
// 2d51): the channel is "present" only when EVERY one of its specs has a
// matching USB device; if any spec is gone the channel is "missing".
//
//   missing ∧ !acked  -> alarming   (speaker alarm active)
//   missing ∧  acked  -> silenced   (stop_alarm; stays silent until a new
//                                    unplug event clears the ack)
class State {
public:
    explicit State(std::vector<CameraSpec> specs);

    const std::vector<CameraSpec>& specs() const { return specs_; }
    // First spec with this channel name (used only for its WAV file names, so
    // with a multi-device channel any one works).
    const CameraSpec* find_spec(const std::string& name) const;

    // ---- startup (main thread, before udev/alarm threads start) ----
    void seed_present(size_t spec_index, const std::string& syspath);

    // ---- udev monitor thread ----
    // A matching USB device appeared. Enqueues a one-shot recover voice when
    // the whole channel transitions missing -> present.
    void add_present_device(size_t spec_index, const std::string& syspath);
    // A USB device was removed (identified by syspath). Returns true when it
    // belonged to a target spec; *removed_name receives the channel name.
    bool remove_device_by_syspath(const std::string& syspath,
                                  std::string* removed_name = nullptr);
    // Fallback when a remove event had no syspath mapping but props matched.
    void remove_by_index(size_t spec_index);

    // ---- UDS control thread ----
    void ack_all_missing();  // stop_alarm: silence every currently-missing channel

    // ---- alarm thread snapshots (channel names, de-duplicated) ----
    std::vector<std::string> missing_specs() const;    // not present
    std::vector<std::string> alarming_specs() const;   // missing ∧ !acked
    std::vector<std::string> present_specs() const;    // all specs present
    bool is_present(const std::string& name) const;    // snapshot test

    // One-shot recover voices produced by add events; consumed by alarm thread.
    std::vector<std::string> take_recover_queue();

    // Interrupt/wake the alarm thread (new unplug, replug, or stop_alarm).
    void request_cancel() { cancel_.store(true); }
    bool take_cancel() { return cancel_.exchange(false); }
    const std::atomic<bool>& cancel_flag() const { return cancel_; }

    // ---- status ----
    std::string status_json() const;

private:
    // Drop every syspath mapping that points at `index` (caller holds m_).
    void clear_syspaths_for(size_t index);
    // True when every spec named `name` has a present device (caller holds m_).
    bool name_present_locked(const std::string& name) const;

    std::vector<CameraSpec> specs_;          // immutable after construction
    std::vector<std::string> channels_;      // unique channel names (spec order)
    mutable std::mutex m_;

    std::map<std::string, size_t> syspath_to_index_;  // syspath -> spec index
    std::set<size_t> present_indices_;                // specs with a present device
    std::set<std::string> acked_;                    // channel names silenced
    std::vector<std::string> recover_queue_;

    std::atomic<bool> cancel_{false};
};
