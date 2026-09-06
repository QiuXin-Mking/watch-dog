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
// Per-camera lifecycle:
//   present (device seen) <-> missing (device gone).
//   missing ∧ !acked  -> alarming   (speaker alarm active)
//   missing ∧  acked  -> silenced   (stop_alarm was used; stays silent until a
//                                    new unplug event clears the ack)
//
// Cameras are tracked per spec name; the syspath->name map lets remove events
// be attributed even when udev remove events no longer carry ID_* properties.
class State {
public:
    explicit State(std::vector<CameraSpec> specs) : specs_(std::move(specs)) {}

    const std::vector<CameraSpec>& specs() const { return specs_; }
    const CameraSpec* find_spec(const std::string& name) const;

    // ---- startup (main thread, before udev/alarm threads start) ----
    void seed_present(const std::string& name, const std::string& syspath);

    // ---- udev monitor thread ----
    // A matching USB device appeared. Returns true when this is a genuine
    // recovery (device was missing), which enqueues a one-shot recover voice.
    void add_present_device(const std::string& name, const std::string& syspath);
    // A USB device was removed; returns true when it belonged to a target spec.
    // If non-null, *removed_name receives the spec name of the removed device.
    bool remove_device_by_syspath(const std::string& syspath,
                                  std::string* removed_name = nullptr);
    // Fallback when a remove event had no syspath mapping but props matched.
    void remove_named(const std::string& name);

    // ---- UDS control thread ----
    void ack_all_missing();  // stop_alarm: silence every currently-missing camera

    // ---- alarm thread snapshots ----
    std::vector<std::string> missing_specs() const;    // present == false
    std::vector<std::string> alarming_specs() const;   // missing ∧ !acked
    std::vector<std::string> present_specs() const;    // present == true
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
    // Drop every syspath mapping that points at `name` (caller holds m_).
    void clear_syspaths_for(const std::string& name);

    std::vector<CameraSpec> specs_;                 // immutable after construction
    mutable std::mutex m_;

    std::map<std::string, std::string> syspath_to_spec_;  // syspath -> spec name
    std::set<std::string> present_;                        // spec names present
    std::set<std::string> acked_;                          // missing + silenced
    std::vector<std::string> recover_queue_;

    std::atomic<bool> cancel_{false};
};
