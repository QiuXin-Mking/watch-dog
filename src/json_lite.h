#pragma once

#include <string>

// Minimal extractor for the flat, single-line JSON objects returned by
// unified_capture. No third-party dependency; only the primitive types we read
// (string / int / bool) are supported. A malformed object or a field with the
// wrong type makes the lookup return false.
namespace json_lite {

bool get_string(const std::string& json, const std::string& key, std::string& out);
bool get_int(const std::string& json, const std::string& key, int& out);
bool get_bool(const std::string& json, const std::string& key, bool& out);

}  // namespace json_lite
