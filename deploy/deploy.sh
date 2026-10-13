#!/usr/bin/env bash
# deploy.sh — build and install watchdog onto this machine.
#
# Run on the RK3588 board from the repo root (02-github/watchdog):
#     bash deploy/deploy.sh            # build + install + (re)start service
#     bash deploy/deploy.sh /tmp/root  # stage into a rootfs (no systemctl)
#
# Idempotent: existing /etc/watchdog/cameras.conf is kept, not overwritten.

set -euo pipefail

cd "$(dirname "$0")/.."

echo "==> make"
make

PREFIX="${1:-}"

echo "==> install binary"
install -D -m 0755 watchdog "${PREFIX}/usr/local/bin/watchdog"

echo "==> install config"
install -d -m 0755 "${PREFIX}/etc/watchdog"
if [[ -f "${PREFIX}/etc/watchdog/cameras.conf" ]]; then
    echo "    keep existing ${PREFIX}/etc/watchdog/cameras.conf"
else
    install -m 0644 deploy/cameras.conf.example "${PREFIX}/etc/watchdog/cameras.conf"
    echo "    installed default cameras.conf"
fi
if [[ -f "${PREFIX}/etc/watchdog/fan.conf" ]]; then
    echo "    keep existing ${PREFIX}/etc/watchdog/fan.conf"
else
    install -m 0644 deploy/fan.conf.example "${PREFIX}/etc/watchdog/fan.conf"
    echo "    installed default fan.conf"
fi

echo "==> install sounds"
install -d -m 0755 "${PREFIX}/etc/watchdog/sounds"
install -m 0644 sounds/*.wav "${PREFIX}/etc/watchdog/sounds/"

echo "==> install systemd unit"
install -D -m 0644 deploy/watchdog.service "${PREFIX}/etc/systemd/system/watchdog.service"

if [[ -z "${PREFIX}" ]]; then
    echo "==> systemctl"
    systemctl daemon-reload
    systemctl enable watchdog.service
    systemctl restart watchdog.service || true
    sleep 1
    systemctl --no-pager --full status watchdog.service || true
    echo "==> smoke: status"
    python3 deploy/uds_cmd.py status || true
fi

echo "done"
