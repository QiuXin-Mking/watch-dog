# watchdog 独立工程 —— USB 摄像头拔线扬声器告警看门狗

> 研发日志子需求 · 对应 Trellis 任务 `09-06-watchdog`（已归档）
> 落地提交：`3a85bbb`（2026-09-06 初始化）、`ff51688`（2026-09-08 六目合并 + 真实语音）· 相关文档：`README.md`、`deploy/cameras.conf.example`
> 核心文件：`src/main.cpp`、`src/state.{h,cpp}`、`src/udev_cameras.{h,cpp}`、`src/audio.{h,cpp}`、`src/uds.{h,cpp}`、`src/wav.{h,cpp}`、`src/spec.{h,cpp}`、`deploy/watchdog.service`

## Goal

让采集设备在 USB 摄像头被物理拔掉（或开机即缺失）时，通过板端 ES8323 扬声器发出**可听的分路语音告警**，使现场操作者立即知道「哪一路摄像头断开」。以**独立工程 `watchdog` / 独立进程**实现，与 unified_capture 完全解耦，覆盖「采集中拔线」「空闲拔线」「开机缺失」三种场景，并提供**停止告警控制接口**供 device-ui 消音。

## Background / 已确认事实

- 需求来源：Trellis 任务 `09-06-watchdog`（prd.md + design.md + implement.md），2026-09-06 立项。
- 板端音频拓扑：播放设备 `plughw:CARD=rockchipes8388,DEV=0`（ES8323 HiFi），带 `Speaker`/`PCM` 混音控制；告警走 ES8323 扬声器，不是麦克风。
- 板端依赖齐备：`libudev.so`、`libasound.so`、`g++` 在位；udev 版本 255；`aplay`/`amixer` 在位。
- 目标摄像头通过 VID/PID + 匹配规则识别；同型号左右腕用 `product` 精确绑定，双目 jhh02（2d50）/四目 jhh04（2d51）用唯一 PID。
- 双目 jhh02 与四目 jhh04 是同一「六目模组」的两个 USB 通道，告警层面合并为一路「六目摄像头」。

## Requirements

### 工程形态与解耦

- 独立 C++17 工程 `watchdog`，`g++ -ludev -lasound -lpthread` 编译产出独立二进制。
- systemd 常驻服务（`watchdog.service`：`Restart=always`、`After=systemd-udevd.service sound.target`），生命周期与 unified_capture 无关。
- 与 unified_capture 零耦合：不共享代码、配置、进程，采集主流程崩溃时告警链路仍存活。

### 检测（libudev 枚举 + 热插拔监听）

- 启动时 `udev_enumerate` 枚举在位 USB 设备（subsystem `usb`、devtype `usb_device`），与「应有清单」比对，缺失路立即入 `missing` 并告警。
- 运行期 `udev_monitor` 监听 usb `add`/`remove`：`remove` 命中目标 → 入 `missing` + 清 ack；`add` 回来 → 出 `missing` + 清 ack + 触发一次「已恢复」语音。
- 匹配规则：VID/PID 必须全等，且配置的 `pid` / `product=<串>` / `serial=<完整SN>` 规则命中；非目标 USB 设备插拔不误报。
- 属性读取以 udev 属性优先、sysfs 属性兜底，新热插拔设备尚未入库时枚举仍能工作。

### 状态机（missing / acked）

- 每路状态机：`present`（在位）↔ `missing`（缺失）；`missing` 时子态 `alarming`（响）与 `acked`（已消音）。
- 告警目标 = `missing ∧ !acked`；`stop_alarm` 对当前所有 missing 置 `acked`（确认/消音），静默直到该路产生新 unplug 事件才重新响。
- 通道聚合：同一 `name` 多行 = 一个通道多设备（六目 = 双目 2d50 + 四目 2d51）；任一行设备缺失该通道即报「断开」，全部在位才判「恢复」。

### 分路语音告警（ALSA → ES8323）

- 语音资源从 `/etc/watchdog/sounds/` 加载（告警 3 + 恢复 3 段 WAV，文案见 voice-script.md）。
- 启动时解析 WAV 头（RIFF/PCM 16-bit S16_LE），把 PCM 载入内存；告警/恢复用 `snd_pcm_writei` 直接写 PCM（`plughw` 自适应格式/采样率/声道转换），无外部解码依赖。
- 单声道 WAV 加载时自动复制为双声道（L=R），保证 ES8323/I2S 双声道 frame slot 下播放速度与音调正常。
- 启动时 `amixer -c 0 sset Speaker 100% unmute` / `PCM 100% unmute`，保证可闻。
- 某 WAV 缺失时该路告警退化为 journald 日志（不 crash）。

### 控制接口（UDS）

- 暴露 `/tmp/watchdog.sock`（Unix 流式 socket，文本 JSON，短连接，与 `/tmp/unified_capture.sock` 同模式）。
- `status` → `{"ok":true,"alarming":...,"missing":[...],"cameras":{...}}`；`stop_alarm` → `{"ok":true}`；未知命令 → `{"ok":false,"error":"unknown command"}`。
- 监听 fd 并入主线程 `poll()`；accept 后 `poll(client, POLLIN, 200ms)` 等待命令，异步客户端（Node net.connect）稳定收发。

### 日志与运维

- 告警/恢复/消音事件写 journald（`journalctl -u watchdog -f`），含时间戳与路名。
- `deploy/deploy.sh` 一键构建 + 安装二进制/配置/语音/systemd 单元 + enable/restart，幂等（保留已有 `cameras.conf`）；`--scan` 枚举在位 USB 设备；`SIGINT/SIGTERM` 优雅退出。

## Acceptance Criteria

- [x] 板端启动看门狗后拔掉任一目标摄像头，扬声器 ≤1s 播放对应「断开」语音（左腕/右腕/六目分路）。
- [x] 空闲态（无采集 session）拔线同样告警，与 unified_capture 无关。
- [x] 开机时某路缺失 → 启动即持续告警；插回后停止。
- [x] 拔非目标 USB 设备（非目标 VID/PID）不触发告警，无误报。
- [x] 停止 unified_capture 服务后看门狗仍能检测拔线并告警。
- [x] 告警持续期间插回该路 → 告警停止并播放对应「已恢复」语音。
- [x] 发 `stop_alarm` → 当前告警立即静音，无新 unplug 事件则不重响。
- [x] 发 `status` → 返回 `alarming`/`missing` 与实际情况一致。

## Key Decisions

1. **独立工程/进程**：看门狗与 unified_capture 完全解耦，采集主流程崩溃时告警链路仍存活，可单独部署、单独回滚（`systemctl disable --now watchdog` 即恢复现状）。
2. **libudev 枚举 + udev_monitor**：一次性覆盖「采集中拔线」「空闲拔线」「开机缺失」三种场景，事件实时且 ID_* 属性已由 udev 处理就绪，高层 API 稳定。
3. **分路语音 WAV 播报**：预录语音清晰可辨，libasound 直写 PCM 无外部解码依赖；告警与恢复各有专属文案，现场可即时区分「哪一路」。
4. **missing/acked 状态机**：`stop_alarm` = 确认/消音语义，操作员可主动消音，新 unplug 事件自动重新告警，交互简洁一致。
5. **UDS 并入主线程 poll**：与 unified_capture 同模式，device-ui 可复用既有 `captureCtl()` 范式接入消音，无独立通信线程。
6. **单声道自动复制双声道**：把「语音文件必须 stereo」的约定内化到播放层，任意 mono 资源都能以正常速度/音调播放，语音资产制备更省心。
