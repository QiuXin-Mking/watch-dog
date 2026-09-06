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

void State::clear_syspaths_for(const std::string& name) {
    for (auto it = syspath_to_spec_.begin(); it != syspath_to_spec_.end();) {
        if (it->second == name) {
            it = syspath_to_spec_.erase(it);
        } else {
            ++it;
        }
    }
}

const CameraSpec* State::find_spec(const std::string& name) const {
    for (const auto& s : specs_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

void State::seed_present(const std::string& name, const std::string& syspath) {
    std::lock_guard<std::mutex> lk(m_);
    // Remove any stale syspath that previously mapped to this spec.
    clear_syspaths_for(name);
    syspath_to_spec_[syspath] = name;
    present_.insert(name);
    acked_.erase(name);
}

void State::add_present_device(const std::string& name, const std::string& syspath) {
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        recovered = (present_.count(name) == 0);
        clear_syspaths_for(name);
        syspath_to_spec_[syspath] = name;
        present_.insert(name);
        acked_.erase(name);
        if (recovered) recover_queue_.push_back(name);
    }
    if (recovered) request_cancel();  // wake alarm thread to announce recovery
}

bool State::remove_device_by_syspath(const std::string& syspath,
                                     std::string* removed_name) {
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = syspath_to_spec_.find(syspath);
        if (it == syspath_to_spec_.end()) return false;
        name = it->second;
        syspath_to_spec_.erase(it);
        present_.erase(name);
        acked_.erase(name);
    }
    if (removed_name) *removed_name = name;
    request_cancel();  // start / restart alarm promptly
    return true;
}

void State::remove_named(const std::string& name) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        // The device is gone; drop any syspath mapping that pointed at it so a
        // later spurious event for that stale path cannot double-remove.
        clear_syspaths_for(name);
        if (present_.erase(name) > 0) {
            acked_.erase(name);
            changed = true;
        }
    }
    if (changed) request_cancel();
}

void State::ack_all_missing() {
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& s : specs_) {
            if (present_.count(s.name) == 0) acked_.insert(s.name);
        }
    }
    request_cancel();  // interrupt any in-flight alarm voice immediately
}

std::vector<std::string> State::missing_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& s : specs_) {
        if (present_.count(s.name) == 0) out.push_back(s.name);
    }
    return out;
}

std::vector<std::string> State::alarming_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& s : specs_) {
        if (present_.count(s.name) == 0 && acked_.count(s.name) == 0) {
            out.push_back(s.name);
        }
    }
    return out;
}

std::vector<std::string> State::present_specs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    for (const auto& s : specs_) {
        if (present_.count(s.name) != 0) out.push_back(s.name);
    }
    return out;
}

bool State::is_present(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_);
    return present_.count(name) != 0;
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
    for (const auto& s : specs_) {
        const bool present = present_.count(s.name) != 0;
        if (!present) {
            if (any_missing) missing += ',';
            missing += '"' + json_escape(s.name) + '"';
            any_missing = true;
            if (acked_.count(s.name) == 0) alarming = true;
        }
    }

    std::string cameras;
    for (const auto& s : specs_) {
        if (!cameras.empty()) cameras += ',';
        cameras += '"' + json_escape(s.name) + "\":" +
                   (present_.count(s.name) ? "true" : "false");
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
