#include "capture_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace {

constexpr size_t kMaxResponseBytes = 4096;
constexpr size_t kReadChunk = 256;

void set_sock_timeout(int fd, int optname, int timeout_ms) {
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

// timeout_ms <= 0 -> blocking connect (legacy). Otherwise non-blocking connect
// bounded by poll(), then the descriptor is restored to blocking I/O.
bool connect_socket(int fd, const struct sockaddr* addr, socklen_t len,
                    int timeout_ms, std::string* error) {
    if (timeout_ms <= 0) {
        if (connect(fd, addr, len) < 0) {
            *error = std::strerror(errno);
            return false;
        }
        return true;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        *error = std::strerror(errno);
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *error = std::strerror(errno);
        return false;
    }

    bool ok = false;
    int rc = connect(fd, addr, len);
    if (rc == 0) {
        ok = true;
    } else if (errno != EINPROGRESS) {
        *error = std::strerror(errno);
    } else {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = 0;
        do {
            pr = poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR);
        if (pr == 0) {
            *error = "connect timeout";
        } else if (pr < 0) {
            *error = std::strerror(errno);
        } else {
            int so_error = 0;
            socklen_t slen = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen) < 0) {
                *error = std::strerror(errno);
            } else if (so_error != 0) {
                *error = std::strerror(so_error);
            } else {
                ok = true;
            }
        }
    }

    // Restore blocking mode so SO_SNDTIMEO/SO_RCVTIMEO apply as before.
    if (fcntl(fd, F_SETFL, flags) < 0) {
        if (ok) {
            *error = std::strerror(errno);
            ok = false;
        }
    }
    return ok;
}

}  // namespace

bool send_command(const std::string& sock_path, const std::string& command,
                  int connect_timeout_ms, int io_timeout_ms,
                  std::string* response, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return fail(std::string("socket() failed: ") + std::strerror(errno));
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return fail("socket path too long: " + sock_path);
    }
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    std::string connect_error;
    if (!connect_socket(fd, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr), connect_timeout_ms, &connect_error)) {
        ::close(fd);
        return fail("connect(" + sock_path + ") failed: " + connect_error);
    }

    set_sock_timeout(fd, SO_SNDTIMEO, io_timeout_ms);
    set_sock_timeout(fd, SO_RCVTIMEO, io_timeout_ms);

    std::string line = command;
    line.push_back('\n');
    const char* ptr = line.data();
    size_t remaining = line.size();
    while (remaining > 0) {
        ssize_t w = write(fd, ptr, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            const std::string message =
                std::string("write failed: ") + std::strerror(errno);
            ::close(fd);
            return fail(message);
        }
        ptr += w;
        remaining -= static_cast<size_t>(w);
    }
    shutdown(fd, SHUT_WR);  // server reads one line then sees EOF

    std::string resp;
    char buf[kReadChunk];
    while (resp.size() < kMaxResponseBytes) {
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
        return fail("no response (timeout or peer closed)");
    }
    if (response) *response = std::move(resp);
    return true;
}
