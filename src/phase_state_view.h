#pragma once

#include <string>
#include <utility>

#include "json_lite.h"

// Snapshot of unified_capture `phase_status`. Only the fields the voice logic
// needs are modelled; `valid` means phase + phase_version were present in a
// well-formed JSON object.
struct PhaseStatus {
    bool valid = false;
    std::string phase;
    int phase_version = -1;
    std::string task_id;
    int threshold_s = 0;
    std::string last_event_id;
    std::string last_source;  // ui | gpio
    std::string last_action;  // zero_start | zero_cancel | prep_start | prep_finish
    std::string last_result;  // ok | invalid_phase | not_claimed | prep_below_threshold
    std::string keys;
    bool blocked = false;
};

// Parse one phase_status response line. Returns false (leaving `out` untouched)
// for malformed JSON, a missing phase/phase_version, or wrong field types.
inline bool parse_phase_status(const std::string& json, PhaseStatus& out) {
    PhaseStatus st;
    if (!json_lite::get_string(json, "phase", st.phase)) return false;
    if (!json_lite::get_int(json, "phase_version", st.phase_version)) return false;

    // Everything else is optional: an older daemon may omit fields.
    json_lite::get_string(json, "task_id", st.task_id);
    json_lite::get_int(json, "threshold_s", st.threshold_s);
    json_lite::get_string(json, "last_event_id", st.last_event_id);
    json_lite::get_string(json, "last_source", st.last_source);
    json_lite::get_string(json, "last_action", st.last_action);
    json_lite::get_string(json, "last_result", st.last_result);
    json_lite::get_string(json, "keys", st.keys);
    json_lite::get_bool(json, "blocked", st.blocked);

    st.valid = true;
    out = std::move(st);
    return true;
}
