#include "capture_ctrl.h"

#include <string>

#include "capture_client.h"
#include "log.h"

CaptureStopResult request_capture_stop(const std::string& sock_path,
                                       int timeout_ms) {
    std::string resp;
    std::string err;
    // connect_timeout_ms = 0 keeps the historical blocking connect; the I/O
    // timeout (15 s from the caller) stays unchanged.
    if (!send_command(sock_path, "stop", 0, timeout_ms, &resp, &err)) {
        log_warn("capture: %s", err.c_str());
        return CaptureStopResult::unreachable;
    }
    if (resp.find("not running") != std::string::npos) {
        return CaptureStopResult::not_running;
    }
    if (resp.find("\"ok\":true") != std::string::npos) {
        return CaptureStopResult::stopped;
    }
    log_warn("capture: unexpected stop response: %s", resp.c_str());
    return CaptureStopResult::unreachable;
}
