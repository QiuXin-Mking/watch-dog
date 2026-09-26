#!/usr/bin/env bash
# play_sounds.sh —— 依次播放自检播报语音，用于板端听感验收。
#
# 用法:
#   bash deploy/play_sounds.sh              # 播放默认 /etc/watchdog/sounds 下的语音
#   bash deploy/play_sounds.sh /some/dir    # 播放指定目录
set -euo pipefail

SOUNDS_DIR="${1:-/etc/watchdog/sounds}"

# 播放顺序：开机成功 → 相机（六目/左右腕）断开+恢复 → 电池/外接电源故障 → 自检通过 → 屏幕故障
FILES=(
  "开机成功.wav"
  "六目断开.wav"
  "六目恢复.wav"
  "左腕断开.wav"
  "左腕恢复.wav"
  "右腕断开.wav"
  "右腕恢复.wav"
  "电池充电异常.wav"
  "外接电源故障.wav"
  "设备自检通过.wav"
  "屏幕故障.wav"
)

echo "sounds dir: ${SOUNDS_DIR}"
for f in "${FILES[@]}"; do
  echo ">>> ${f}"
  if [[ -f "${SOUNDS_DIR}/${f}" ]]; then
    aplay -q "${SOUNDS_DIR}/${f}"
  else
    echo "    (missing)"
  fi
  sleep 0.5
done
echo "done."
