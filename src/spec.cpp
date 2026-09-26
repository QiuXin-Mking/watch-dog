#include "spec.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
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

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool parse_hex16(const std::string& s_in, uint16_t& out) {
    if (s_in.empty()) return false;
    std::string s = trim(s_in);
    char* end = nullptr;
    errno = 0;
    unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    if (v > 0xffff) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

std::vector<CameraSpec> parse_cameras_conf(const std::string& path, std::string& error) {
    std::vector<CameraSpec> specs;
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return {};
    }

    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        // Strip '#' comment.
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;

        std::vector<std::string> tok = split_ws(line);
        // name vid pid match_rule [alarm_wav recover_wav]
        if (tok.size() < 4) {
            std::ostringstream oss;
            oss << path << ':' << line_no << ": expected 'name vid pid match_rule [alarm_wav recover_wav]'";
            error = oss.str();
            return {};
        }

        CameraSpec spec;
        spec.name = tok[0];
        // A name may repeat: each line declares one device of a channel. A
        // channel backed by several devices (e.g. "sixcam" = jhh02 + jhh04) is
        // present only when every one of its devices is present (see state.h).
        if (!parse_hex16(tok[1], spec.vid) || spec.vid == 0) {
            std::ostringstream oss;
            oss << path << ':' << line_no << ": bad vid '" << tok[1] << "'";
            error = oss.str();
            return {};
        }
        if (!parse_hex16(tok[2], spec.pid) || spec.pid == 0) {
            std::ostringstream oss;
            oss << path << ':' << line_no << ": bad pid '" << tok[2] << "'";
            error = oss.str();
            return {};
        }

        // match_rule: pid | product=<string> | serial=<full-string>
        if (tok[3] == "pid") {
            spec.match.type = MatchRule::Type::kPid;
        } else if (starts_with(tok[3], "product=")) {
            spec.match.type = MatchRule::Type::kProduct;
            spec.match.value = tok[3].substr(std::string("product=").size());
            if (spec.match.value.empty()) {
                std::ostringstream oss;
                oss << path << ':' << line_no << ": empty product match";
                error = oss.str();
                return {};
            }
        } else if (starts_with(tok[3], "serial=")) {
            spec.match.type = MatchRule::Type::kSerial;
            spec.match.value = tok[3].substr(std::string("serial=").size());
            if (spec.match.value.empty()) {
                std::ostringstream oss;
                oss << path << ':' << line_no << ": empty serial match";
                error = oss.str();
                return {};
            }
        } else {
            std::ostringstream oss;
            oss << path << ':' << line_no << ": match_rule must be 'pid', 'product=<string>', or 'serial=<string>'";
            error = oss.str();
            return {};
        }

        // WAV file names (optional, sensible defaults).
        if (tok.size() >= 5) {
            spec.alarm_wav = tok[4];
        } else {
            spec.alarm_wav = spec.name + ".wav";
        }
        if (tok.size() >= 6) {
            spec.recover_wav = tok[5];
        } else {
            spec.recover_wav = spec.name + "_recover.wav";
        }

        specs.push_back(std::move(spec));
    }

    if (specs.empty()) {
        error = path + ": no camera entries found";
        return {};
    }
    return specs;
}
