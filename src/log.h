#pragma once

// Minimal timestamped logging. Running under systemd, stdout/stderr are
// captured by journald (journalctl -u watchdog -f).

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

namespace watchdog_log {

inline std::string now_str() {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "%F %T", &tmv);
    return std::string(buf);
}

inline void log_line(const char* level, const char* fmt, va_list ap) {
    std::fprintf(stdout, "[watchdog %s] %s ", now_str().c_str(), level);
    std::vfprintf(stdout, fmt, ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace watchdog_log

inline void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    watchdog_log::log_line("INFO", fmt, ap);
    va_end(ap);
}

inline void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    watchdog_log::log_line("WARN", fmt, ap);
    va_end(ap);
}

inline void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    watchdog_log::log_line("ERROR", fmt, ap);
    va_end(ap);
}
