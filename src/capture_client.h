#pragma once

#include <string>

// One-shot command client for the unified_capture control socket: connect,
// write "command\n", shutdown the write side, read one response line, close.
//
// connect_timeout_ms <= 0 keeps a plain blocking connect (the historical
// request_capture_stop behavior); > 0 bounds connect with poll(). io_timeout_ms
// bounds both the write and the read. Returns false on any failure and fills
// *error (when non-null) with a human-readable reason.
bool send_command(const std::string& sock_path, const std::string& command,
                  int connect_timeout_ms, int io_timeout_ms,
                  std::string* response, std::string* error);
