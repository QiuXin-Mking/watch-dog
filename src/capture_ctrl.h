#pragma once

#include <string>

// Outcome of asking unified_capture to stop its current session so its data can
// be finalized before we power the device off.
enum class CaptureStopResult {
    stopped,      // was recording; teardown + manifest finalize completed
    not_running,  // nothing was recording
    unreachable,  // socket missing / connect failed / I/O error / timeout
};

// Connect to the unified_capture control socket at `sock_path`, send "stop" and
// read one response line. unified_capture's `stop` handler blocks until the
// session has fully torn down (MKV close + finalize), so a normal return means
// the on-disk data is complete; this call blocks up to `timeout_ms` for it.
// Never throws and never blocks forever: on any failure it returns unreachable
// so the caller can keep going with the shutdown.
CaptureStopResult request_capture_stop(const std::string& sock_path,
                                       int timeout_ms);
