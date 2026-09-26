#include "uds.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "log.h"

UdsServer::~UdsServer() {
    close();
}

bool UdsServer::open() {
    if (fd_ >= 0) return true;

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        log_error("uds: socket() failed: %s", std::strerror(errno));
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    // Unlink a stale socket left by a previous run, but only if nothing is
    // listening on it right now (avoids stepping on a live second instance).
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)) < 0) {
            unlink(path_.c_str());
        }
        ::close(probe);
    }

    if (bind(server, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("uds: bind(%s) failed: %s (another watchdog running?)",
                  path_.c_str(), std::strerror(errno));
        ::close(server);
        return false;
    }
    if (listen(server, 4) < 0) {
        log_error("uds: listen(%s) failed: %s", path_.c_str(),
                  std::strerror(errno));
        ::close(server);
        unlink(path_.c_str());
        return false;
    }

    int flags = fcntl(server, F_GETFL, 0);
    fcntl(server, F_SETFL, flags | O_NONBLOCK);
    fd_ = server;
    log_info("uds: listening on %s", path_.c_str());
    return true;
}

void UdsServer::close() {
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
    unlink(path_.c_str());
}

void UdsServer::serve_one(
    const std::function<std::string(const std::string&)>& handler) {
    if (fd_ < 0) return;

    int client = accept(fd_, nullptr, nullptr);
    if (client < 0) return;

    // The client socket does not inherit O_NONBLOCK from the listener. Make it
    // non-blocking so a peer that connects but never writes cannot stall the
    // main control loop (and with it signal handling / shutdown).
    int fl = fcntl(client, F_GETFL, 0);
    if (fl >= 0) fcntl(client, F_SETFL, fl | O_NONBLOCK);

    // Wait briefly for the peer's one-line command before reading. The client
    // socket is non-blocking above, so an async client (Node net.connect + a
    // 'connect'-callback write) would otherwise race: read() EAGAINs on an
    // empty socket, we close it, and the client's later write hits EPIPE —
    // which made the device-ui self-check read the watchdog as offline. poll()
    // gives the peer a short window to send its command.
    struct pollfd pfd { client, POLLIN, 0 };
    if (poll(&pfd, 1, 200) <= 0) {
        ::close(client);
        return;  // no command within 200ms: stray/aborted client
    }

    // Read one command line (short connection: client sends one line then
    // closes / shutdown(SHUT_WR)). Loop to tolerate partial reads.
    std::string cmd;
    char buf[128];
    while (cmd.size() < 1024) {
        ssize_t n = read(client, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN / error: whatever we have is the whole request
        }
        if (n == 0) break;  // peer closed its write side
        cmd.append(buf, static_cast<size_t>(n));
        if (cmd.find('\n') != std::string::npos) break;
    }

    if (!cmd.empty()) {
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
            cmd.pop_back();
        }
        std::string resp = handler(cmd);
        resp += '\n';

        const char* data = resp.data();
        size_t remaining = resp.size();
        while (remaining > 0) {
            ssize_t w = write(client, data, remaining);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;  // peer gone / EAGAIN: drop the rest of the response
            }
            data += w;
            remaining -= static_cast<size_t>(w);
        }
    }
    ::close(client);
}
