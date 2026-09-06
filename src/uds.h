#pragma once

#include <functional>
#include <string>

// Unix domain socket control interface, same pattern as
// /tmp/unified_capture.sock: streaming socket, one text line per command,
// short connection (server reads one command, writes one response, closes).
class UdsServer {
public:
    explicit UdsServer(std::string path) : path_(std::move(path)) {}
    ~UdsServer();

    UdsServer(const UdsServer&) = delete;
    UdsServer& operator=(const UdsServer&) = delete;

    bool open();
    int fd() const { return fd_; }
    void close();

    // Accept one pending client (non-blocking) and serve it.
    void serve_one(const std::function<std::string(const std::string&)>& handler);

private:
    std::string path_;
    int fd_ = -1;
};
