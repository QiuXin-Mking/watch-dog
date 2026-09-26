# watchdog — USB 摄像头拔线扬声器告警看门狗

独立常驻进程，监听板端 USB 摄像头热插拔；目标摄像头被拔掉（或开机即缺失）时通过
ES8323 扬声器循环播放对应「断开」语音，插回后播放「已恢复」语音，并提供
`/tmp/watchdog.sock` 控制接口供 device-ui 消音/查询。

与 `unified_capture` 完全解耦：不共享代码/配置/进程，采集崩溃也能告警。

## 架构

```
watchdog (systemd 常驻服务, Restart=always)
├── 启动: 读 /etc/watchdog/cameras.conf → libudev 枚举在位摄像头 → 比对清单
├── 运行: udev_monitor 监听 usb add/remove → 维护 missing/acked 状态
├── 告警: alarm 线程对 missing∧!acked 各路循环播放分路语音 (libasound → ES8323)
└── 控制: UDS /tmp/watchdog.sock → status / stop_alarm
```

- 2 线程：udev 事件线程 + alarm 播放线程；共享状态由 mutex 保护。
- 每路摄像头状态机：`present ⇄ missing`；`missing` 时子态 `alarming`（响）/
  `acked`（已消音）。`stop_alarm` = 对当前所有 missing 置 `acked`（确认/消音），
  静默直到该路产生新 unplug 事件才重新响。
- UDS 监听 fd 并入主线程 `poll()`（与 `/tmp/unified_capture.sock` 同模式）。
- 日志走 journald（`journalctl -u watchdog -f`）：告警/恢复/消音各一条含时间戳与路名。

## 目录

```
src/                 C++17 源码
  main.cpp           装配 + 主 poll 循环 + alarm/udev 线程
  spec.{h,cpp}       cameras.conf 解析
  state.{h,cpp}      missing/acked/acked 共享状态 + status JSON
  udev_cameras.{h,cpp} libudev 枚举/监听/匹配
  wav.{h,cpp}        RIFF/WAV (PCM16 mono) 解析
  audio.{h,cpp}      SoundBank 预载 + ALSA (plughw) 播放
  uds.{h,cpp}        /tmp/watchdog.sock 短连接控制服务
deploy/
  cameras.conf.example  应有清单 (mango_pro 四路)
  watchdog.service      systemd 单元
  deploy.sh             一键部署脚本 (板上执行)
sounds/                  8 个占位 WAV + gen_placeholder_wav.py
Makefile              make / make clean
```

## 编译（必须在板端，Mac 无 libudev/libasound）

```bash
# 把本目录拷到板端后
make            # 产出 ./watchdog (g++ -ludev -lasound -lpthread)
make clean
```

## 匹配规则

每路 `CameraSpec` 声明 vid、pid 和一个匹配规则。设备必须先满足 VID/PID，
再满足 `serial=<完整SN>`、`product=<string>` 或 `pid` 规则。当前同型号左右腕
使用完整 SN 精确绑定；jhh02/jhh04 的 PID 唯一，使用 `pid` 规则。

## 配置格式

`/etc/watchdog/cameras.conf`，纯文本、`#` 注释、空白分隔，每行一路：

```
# name      vid   pid  match_rule                              alarm_wav       recover_wav
wrist_left  1bcf  2d52 serial=JH01260911A01W10FV200C1A01217 wrist_left.wav  wrist_left_recover.wav
wrist_right 1bcf  2d52 serial=JH01260911A01W10FV200C1A01670 wrist_right.wav wrist_right_recover.wav
jhh02       1bcf  2d50 pid                                     jhh02.wav       jhh02_recover.wav
jhh04       1bcf  2d51 pid                                     jhh04.wav       jhh04_recover.wav
```

- `match_rule`：`serial=<完整SN>`、`product=<string>` 或 `pid`。
- WAV 列可省略，缺省 `<name>.wav` / `<name>_recover.wav`。

## 语音资源

`/etc/watchdog/sounds/` 下 8 个 WAV（PCM16 单声道 48000Hz）：

| 告警 | 恢复 |
|------|------|
| wrist_left.wav（左腕断开） | wrist_left_recover.wav（左腕已恢复） |
| wrist_right.wav（右腕断开） | wrist_right_recover.wav（右腕已恢复） |
| jhh02.wav（双目断开） | jhh02_recover.wav（双目已恢复） |
| jhh04.wav（四目断开） | jhh04_recover.wav（四目已恢复） |

当前为占位正弦短音（告警 440/660/880/1100 Hz，恢复更低频），真实语音后续替换为
`voice-script.md` 文案录音，同名即可。某 WAV 缺失时该路退化为 journald 日志（不 crash）。

## 控制接口（UDS 契约）

`/tmp/watchdog.sock`，Unix 流式 socket、文本 JSON、每条命令一行、短连接。

板端无 netcat，用仓库自带助手 `deploy/uds_cmd.py`：

```bash
python3 deploy/uds_cmd.py status
# {"ok":true,"alarming":false,"missing":[],"cameras":{"wrist_left":true,"wrist_right":true,"jhh02":true,"jhh04":true}}

python3 deploy/uds_cmd.py stop_alarm
# {"ok":true}   (消音当前所有 missing；新 unplug 才重新响)
```

装了 netcat 的机器也可用 `echo status | nc -U /tmp/watchdog.sock`。

- `status` → `ok / alarming / missing / cameras`（每路是否在位）。
- `stop_alarm` → 静音所有当前告警，语义 D5（确认/消音）。
- 未知命令 → `{"ok":false,"error":"unknown command"}`。

## 部署（systemd）

```bash
# 板上仓库根目录执行（会 make + 安装 + 启动服务）
bash deploy/deploy.sh

systemctl status watchdog          # 状态
journalctl -u watchdog -f          # 日志
systemctl disable --now watchdog   # 回滚/卸载
```

`watchdog.service`：`ExecStart=/usr/local/bin/watchdog /etc/watchdog/cameras.conf`，
`Restart=always`，`After=systemd-udevd.service sound.target`。

## 排障

- 扬声器出声：`speaker-test -D plughw:CARD=rockchipes8388,DEV=0 -t sine -f 880 -l 1`
- 静音/音量：`amixer -c 0 sset Speaker 100% unmute`；`amixer -c 0 sset PCM 100% unmute`
- 看当前在位设备：`watchdog --scan`
- 热插拔事件：`udevadm monitor -u -s usb`
- 直接前台调试（不走 systemd）：
  ```bash
  /usr/local/bin/watchdog /etc/watchdog/cameras.conf
  # 或指定不同配置/声音目录/ALSA 设备:
  ./watchdog ./deploy/cameras.conf.example ./sounds 'plughw:CARD=rockchipes8388,DEV=0'
  ```

## 已知限制 / 待办

- 物理拔插测试（AC1–AC6）需真人操作，留待板端联调。
- device-ui 侧「告警横幅 + 消音按钮」未实现（已记 `02-github/device-ui/TODO.md` 代办）。
