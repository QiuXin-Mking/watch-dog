#!/usr/bin/env bash
# play_calibration_sounds.sh —— 顺序播放 F-05 标定按键播报，用于板端听感验收。
#
# 用法:
#   bash deploy/play_calibration_sounds.sh              # 播放默认 /etc/watchdog/sounds 下的语音
#   bash deploy/play_calibration_sounds.sh /some/dir    # 播放指定目录
#
# 触发规则见合同文档 F05-标定录制-按键播报补充需求-给watchdog.md。
set -euo pipefail

SOUNDS_DIR="${1:-/etc/watchdog/sounds}"

# 播放顺序：key1 开始 → 取消 → key2 开始 → 标零完成提示 → 失败/引导 → 标定完成
FILES=(
  "标定已开始，请保持静止.wav"
  "标定已取消.wav"
  "准备动作录制中.wav"
  "标0完成，请穿戴设备，录制准备动作.wav"
  "当前不能开始标定.wav"
  "未按要求执行准备动作录制.wav"
  "请先选择任务.wav"
  "设备正在收尾，请稍后.wav"
  "标定完成.wav"
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
