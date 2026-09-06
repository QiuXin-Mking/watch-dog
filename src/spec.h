#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One line in cameras.conf declares one expected camera ("应有清单").
//
//   # name      vid   pid  sn_prefix  fallback        alarm_wav          recover_wav
//   wrist_left  1bcf  2d52  SL         product=SL      wrist_left.wav     wrist_left_recover.wav
//
// Matching rule (vid+pid must always match):
//   serial starts with sn_prefix            -- new firmware SN (SL/SR/ST/SF)
//   OR fallback:
//        product=<string> : udev ID_MODEL exactly equals <string>   (old firmware)
//        pid              : vid+pid alone is enough                  (unique PID)
struct FallbackRule {
    enum class Type { kPid, kProduct };
    Type type = Type::kPid;
    std::string product;  // valid when type == kProduct
};

struct CameraSpec {
    std::string name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string sn_prefix;  // empty / "-" means no serial-prefix rule
    FallbackRule fallback;
    std::string alarm_wav;    // default: <name>.wav
    std::string recover_wav;  // default: <name>_recover.wav
};

// Parse a hex string like "1bcf" / "2d52" into a uint16_t.
bool parse_hex16(const std::string& s, uint16_t& out);

// Parse cameras.conf. Blank lines and '#' comments are ignored.
// On the first malformed line, *error is filled (with file:line) and {} is returned.
std::vector<CameraSpec> parse_cameras_conf(const std::string& path, std::string& error);
