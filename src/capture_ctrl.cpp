#include "capture_ctrl.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "log.h"

namespace {

void set_sock_timeout(int fd, int optname, int timeout_ms) {
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

}  // namespace

CaptureStopResult request_capture_stop(const std::string& sock_path,
                                       int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_warn("capture: socket() failed: %s", std::strerror(errno));
        return CaptureStopResult::unreachable;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(addr.sun_path)) {
        log_warn("capture: socket path too long: %s", sock_path.c_str());
        ::close(fd);
        return CaptureStopResult::unreachable;
    }
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_warn("capture: connect(%s) failed: %s", sock_path.c_str(),
                 std::strerror(errno));
        ::close(fd);
        return CaptureStopResult::unreachable;
    }

    set_sock_timeout(fd, SO_SNDTIMEO, timeout_ms);
    set_sock_timeout(fd, SO_RCVTIMEO, timeout_ms);

    const char* cmd = "stop\n";
    size_t remaining = std::strlen(cmd);
    while (remaining > 0) {
        ssize_t w = write(fd, cmd, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            log_warn("capture: write(stop) failed: %s", std::strerror(errno));
            ::close(fd);
            return CaptureStopResult::unreachable;
        }
        cmd += w;
        remaining -= static_cast<size_t>(w);
    }
    shutdown(fd, SHUT_WR);  // server reads one line then sees EOF

    std::string resp;
    char buf[256];
    while (resp.size() < 4096) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN (timeout) or error: use whatever we have
        }
        if (n == 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find('\n') != std::string::npos) break;
    }
    ::close(fd);

    if (resp.empty()) {
        log_warn("capture: no response to stop (timeout or peer closed)");
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
