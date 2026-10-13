#include "sensor.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strict decimal parse: false on empty / trailing junk / overflow.
bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace

FanConfig parse_fan_conf(const std::string& path) {
    FanConfig cfg;
    std::ifstream in(path);
    if (!in) return cfg;  // missing file -> all defaults

    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        int iv = 0;
        if (key == "overtemp_threshold_mc" && parse_int(val, iv) && iv > 0) {
            cfg.overtemp_threshold_mc = iv;
        } else if (key == "stall_seconds" && parse_int(val, iv) && iv > 0) {
            cfg.stall_seconds = iv;
        } else if (key == "poll_seconds" && parse_int(val, iv) && iv > 0) {
            cfg.poll_seconds = iv;
        } else if (key == "fan_hwmon_name" && !val.empty()) {
            cfg.fan_hwmon_name = val;
        } else if (key == "thermal_zone_type" && !val.empty()) {
            cfg.thermal_zone_type = val;
        } else if (key == "fan_alarm_wav" && !val.empty()) {
            cfg.fan_alarm_wav = val;
        } else if (key == "fan_recover_wav" && !val.empty()) {
            cfg.fan_recover_wav = val;
        } else if (key == "overtemp_alarm_wav" && !val.empty()) {
            cfg.overtemp_alarm_wav = val;
        } else if (key == "overtemp_recover_wav" && !val.empty()) {
            cfg.overtemp_recover_wav = val;
        }
        // unknown keys are ignored.
    }
    return cfg;
}
