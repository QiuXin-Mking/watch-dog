#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One line in cameras.conf declares one expected camera ("应有清单").
//
//   # name      vid   pid  match_rule                              alarm_wav       recover_wav
//   wrist_left  1bcf  2d52 serial=<full-device-serial>             wrist_left.wav  wrist_left_recover.wav
//
// Matching rule (vid+pid must always match):
//   serial=<string>  : USB serial exactly equals <string>
//   product=<string> : udev ID_MODEL exactly equals <string>
//   pid              : vid+pid alone is enough
struct MatchRule {
    enum class Type { kPid, kProduct, kSerial };
    Type type = Type::kPid;
    std::string value;
};

struct CameraSpec {
    std::string name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    MatchRule match;
    std::string alarm_wav;    // default: <name>.wav
    std::string recover_wav;  // default: <name>_recover.wav
};

// Parse a hex string like "1bcf" / "2d52" into a uint16_t.
bool parse_hex16(const std::string& s, uint16_t& out);

// Parse cameras.conf. Blank lines and '#' comments are ignored.
// On the first malformed line, *error is filled (with file:line) and {} is returned.
std::vector<CameraSpec> parse_cameras_conf(const std::string& path, std::string& error);
