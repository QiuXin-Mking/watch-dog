#include "state.h"

#include <cstdio>
#include <utility>

namespace {

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

}  // namespace

State::State(std::vector<CameraSpec> specs, std::vector<SensorChannel> sensors)
    : specs_(std::move(specs)), sensors_(std::move(sensors)) {
    for (const auto& s : specs_) {
        bool seen = false;
        for (const auto& c : channels_) {
            if (c == s.name) {
                seen = true;
                break;
            }
        }
        if (!seen) channels_.push_back(s.name);
    }
    for (const auto& s : sensors_) {
        bool seen = false;
        for (const auto& c : channels_) {
            if (c == s.name) {
                seen = true;
                break;
            }
        }
        if (!seen) channels_.push_back(s.name);
        // Assume healthy until the first fan-loop poll reports the real state,
        // so a sensor can never alarm before its sysfs has been read once.
        sensor_present_[s.name] = true;
    }
}

void State::clear_syspaths_for(size_t index) {
    for (auto it = syspath_to_index_.begin(); it != syspath_to_index_.end();) {
        if (it->second == index) {
            it = syspath_to_index_.erase(it);
        } else {
            ++it;
        }
    }
}

const std::string* State::alarm_wav_for(const std::string& name) const {
    for (const auto& s : sensors_) {
        if (s.name == name) return &s.alarm_wav;
    }
    for (const auto& s : specs_) {
        if (s.name == name) return &s.alarm_wav;
    }
    return nullptr;
}

const std::string* State::recover_wav_for(const std::string& name) const {
    for (const auto& s : sensors_) {
        if (s.name == name) return &s.recover_wav;
    }
    for (const auto& s : specs_) {
        if (s.name == name) return &s.recover_wav;
    }
    return nullptr;
}

bool State::name_present_locked(const std::string& name) const {
    auto sit = sensor_present_.find(name);
    if (sit != sensor_present_.end()) return sit->second;

    bool any = false;
    for (size_t i = 0; i < specs_.size(); ++i) {
        if (specs_[i].name != name) continue;
        any = true;
        if (present_indices_.count(i) == 0) return false;
    }
    return any;  // false when the name doesn't exist at all
}

void State::seed_present(size_t spec_index, const std::string& syspath) {
    std::lock_guard<std::mutex> lk(m_);
    // Remove any stale syspath that previously mapped to this spec.
    clear_syspaths_for(spec_index);
    syspath_to_index_[syspath] = spec_index;
    present_indices_.insert(spec_index);
    acked_.erase(specs_[spec_index].name);
}

void State::add_present_device(size_t spec_index, const std::string& syspath) {
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const std::string name = specs_[spec_index].name;
        const bool was_present = name_present_locked(name);
        clear_syspaths_for(spec_index);
        syspath_to_index_[syspath] = spec_index;
        present_indices_.insert(spec_index);
        acked_.erase(name);
        const bool now_present = name_present_locked(name);
        recovered = !was_present && now_present;
        if (recovered) recover_queue_.push_back(name);
    }
    if (recovered) request_cancel();  // wake alarm thread to announce recovery
}

bool State::remove_device_by_syspath(const std::string& syspath,
                                     std::string* removed_name) {
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = syspath_to_index_.find(syspath);
        if (it == syspath_to_index_.end()) return false;
        const size_t index = it->second;
        name = specs_[index].name;
        syspath_to_index_.erase(it);
        present_indices_.erase(index);
        acked_.erase(name);  // a re-unplug re-arms the alarm
    }
    if (removed_name) *removed_name = name;
    request_cancel();  // start / restart alarm promptly
    return true;
}

void State::remove_by_index(size_t spec_index) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        // The device is gone; drop any syspath mapping that pointed at it so a
        // later spurious event for that stale path cannot double-remove.
        clear_syspaths_for(spec_index);
        if (present_indices_.erase(spec_index) > 0) {
            acked_.erase(specs_[spec_index].name);
            changed = true;
        }
    }
    if (changed) request_cancel();
}

void State::set_sensor_present(const std::string& name, bool healthy) {
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = sensor_present_.find(name);
        if (it == sensor_present_.end()) return;  // unknown sensor channel
        if (it->second == healthy) return;        // no transition
        it->second = healthy;
        acked_.erase(name);  // both edges re-arm / clear the ack (D4 semantics)
        if (healthy) recover_queue_.push_back(name);
    }
    request_cancel();  // wake the alarm thread to (un)alarm promptly
}

void State::ack_all_missing() {
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& name : channels_) {
            if (!name_present_locked(name)) acked_.insert(name);
        }
    }
    request_cancel();  // interrupt any in-flight alarm voice immediately
}

std::vector<std::string> State::missing_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& name : channels_) {
        if (!name_present_locked(name)) out.push_back(name);
    }
    return out;
}

std::vector<std::string> State::alarming_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& name : channels_) {
        if (!name_present_locked(name) && acked_.count(name) == 0) {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<std::string> State::present_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& name : channels_) {
        if (name_present_locked(name)) out.push_back(name);
    }
    return out;
}

bool State::is_present(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_);
    return name_present_locked(name);
}

std::vector<std::string> State::take_recover_queue() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out = std::move(recover_queue_);
    recover_queue_.clear();
    return out;
}

std::string State::status_json() const {
    std::lock_guard<std::mutex> lk(m_);

    std::string missing;
    bool any_missing = false;
    bool alarming = false;
    for (const auto& name : channels_) {
        const bool present = name_present_locked(name);
        if (!present) {
            if (any_missing) missing += ',';
            missing += '"' + json_escape(name) + '"';
            any_missing = true;
            if (acked_.count(name) == 0) alarming = true;
        }
    }

    std::string cameras;
    for (const auto& name : channels_) {
        if (!cameras.empty()) cameras += ',';
        cameras += '"' + json_escape(name) + "\":" +
                   (name_present_locked(name) ? "true" : "false");
    }

    std::string out = "{\"ok\":true,\"alarming\":";
    out += alarming ? "true" : "false";
    out += ",\"missing\":[";
    out += missing;
    out += "],\"cameras\":{";
    out += cameras;
    out += "}}";
    return out;
}
