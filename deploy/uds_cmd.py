#!/usr/bin/env python3
"""Send one command to the watchdog UDS and print the JSON response line.

Stand-in for `nc -U /tmp/watchdog.sock` (netcat is not installed on the board).

    usage: python3 uds_cmd.py status
           python3 uds_cmd.py stop_alarm
"""

import socket
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: uds_cmd.py <status|stop_alarm>", file=sys.stderr)
        return 2
    cmd = sys.argv[1]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect("/tmp/watchdog.sock")
    s.sendall((cmd + "\n").encode("utf-8"))
    s.shutdown(socket.SHUT_WR)  # tell the server we are done sending

    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()

    sys.stdout.write(data.decode("utf-8", "replace"))
    if not data.endswith(b"\n"):
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
