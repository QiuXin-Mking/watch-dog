#include "json_lite.h"

#include <cctype>
#include <cstdint>
#include <utility>

namespace json_lite {
namespace {

void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// Cursor over a flat JSON object. Values for unknown keys are skipped with
// balanced-brace tracking so nested objects/arrays never confuse the scan.
class Cursor {
public:
    explicit Cursor(const std::string& text) : s_(text) {}

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' ||
                                  s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    bool eat(char c) {
        ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool at_end() const { return i_ >= s_.size(); }

    bool parse_string(std::string& out) {
        ws();
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                        else return false;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_int(int& out) {
        ws();
        bool negative = false;
        if (i_ < s_.size() && s_[i_] == '-') {
            negative = true;
            ++i_;
        }
        if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            return false;
        }
        long long value = 0;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            value = value * 10 + (s_[i_] - '0');
            if (value > 0x7fffffffLL) return false;
            ++i_;
        }
        out = static_cast<int>(negative ? -value : value);
        return true;
    }

    bool parse_bool(bool& out) {
        ws();
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            out = true;
            return true;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool skip_value() {
        ws();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (s_[i_] == '{' || s_[i_] == '[') return skip_container();
        const size_t start = i_;
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
               !std::isspace(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
        }
        return i_ > start;
    }

private:
    bool skip_container() {
        std::string stack(1, s_[i_++]);
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') {
                std::string ignored;
                if (!parse_string(ignored)) return false;
                continue;
            }
            if (c == '{' || c == '[') {
                stack.push_back(c);
                ++i_;
                continue;
            }
            if (c == '}' || c == ']') {
                if (stack.empty()) return false;
                const char open = stack.back();
                if ((c == '}' && open != '{') || (c == ']' && open != '[')) return false;
                stack.pop_back();
                ++i_;
                if (stack.empty()) return true;
                continue;
            }
            ++i_;
        }
        return false;
    }

    const std::string& s_;
    size_t i_ = 0;
};

enum class Kind { kString, kInt, kBool };

bool find_field(const std::string& text, const std::string& key, Kind kind,
                std::string* str_out, int* int_out, bool* bool_out) {
    Cursor c(text);
    if (!c.eat('{')) return false;
    bool found = false;
    if (!c.eat('}')) {
        while (true) {
            std::string k;
            if (!c.parse_string(k)) return false;
            if (!c.eat(':')) return false;
            if (k == key) {
                bool parsed = false;
                if (kind == Kind::kString) parsed = c.parse_string(*str_out);
                else if (kind == Kind::kInt) parsed = c.parse_int(*int_out);
                else parsed = c.parse_bool(*bool_out);
                if (!parsed) return false;
                found = true;
            } else if (!c.skip_value()) {
                return false;
            }
            if (c.eat(',')) continue;
            if (c.eat('}')) break;
            return false;
        }
    }
    if (!found) return false;
    // Reject trailing garbage after the closing brace so a truncated stream
    // that happens to contain the key is not mistaken for valid JSON.
    c.ws();
    return c.at_end();
}

}  // namespace

bool get_string(const std::string& json, const std::string& key, std::string& out) {
    std::string value;
    if (!find_field(json, key, Kind::kString, &value, nullptr, nullptr)) return false;
    out = std::move(value);
    return true;
}

bool get_int(const std::string& json, const std::string& key, int& out) {
    int value = 0;
    if (!find_field(json, key, Kind::kInt, nullptr, &value, nullptr)) return false;
    out = value;
    return true;
}

bool get_bool(const std::string& json, const std::string& key, bool& out) {
    bool value = false;
    if (!find_field(json, key, Kind::kBool, nullptr, nullptr, &value)) return false;
    out = value;
    return true;
}

}  // namespace json_lite
