#!/usr/bin/env python3
"""用 edge-tts 生成 watchdog 播报语音。

链路：文案 → edge-tts（微软在线合成）→ MP3 → miniaudio 解码成
44.1kHz 双声道 S16 → 峰值归一化 → sounds/<文件名>.wav

生成发生在开发机，板端播放不依赖 Python。产物提交进仓库，由
deploy/deploy.sh 同步到 /etc/watchdog/sounds/。

依赖（装在工作区外，勿污染项目）：
    python3 -m venv ~/.cache/watchdog-tts-venv
    ~/.cache/watchdog-tts-venv/bin/pip install edge-tts miniaudio

用法：
    ~/.cache/watchdog-tts-venv/bin/python tools/gen_voices.py
    ~/.cache/watchdog-tts-venv/bin/python tools/gen_voices.py --only 风扇故障.wav 过温.wav
    ~/.cache/watchdog-tts-venv/bin/python tools/gen_voices.py --voice zh-CN-XiaoxiaoNeural

完整说明见 docs/03-语音合成流水线.md。
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import wave
from array import array
from dataclasses import dataclass
from pathlib import Path

try:
    import edge_tts
    import miniaudio
except ImportError:  # noqa: BLE001 - 给操作者看的提示，不是库错误
    sys.exit(
        "缺少 edge-tts/miniaudio。请在工作区外建 venv 安装：\n"
        "  python3 -m venv ~/.cache/watchdog-tts-venv\n"
        "  ~/.cache/watchdog-tts-venv/bin/pip install edge-tts miniaudio"
    )

DEFAULT_VOICE = "zh-CN-XiaoyiNeural"
SAMPLE_RATE = 44100
CHANNELS = 2
TARGET_PEAK = 0.89  # 与 whale-live-bridge 一致：44% → 89%（约 +6dB）
MAX_GAIN = 4.0
OUT_DIR = Path(__file__).resolve().parent.parent / "sounds"


@dataclass(frozen=True)
class Voice:
    filename: str
    text: str


# 文件名词条必须与代码/配置里的引用逐字一致。
VOICES: tuple[Voice, ...] = (
    Voice("开机成功.wav", "开机成功"),
    Voice("六目断开.wav", "六目摄像头断开"),
    Voice("六目恢复.wav", "六目摄像头已恢复"),
    Voice("左腕断开.wav", "左腕摄像头断开"),
    Voice("左腕恢复.wav", "左腕摄像头已恢复"),
    Voice("右腕断开.wav", "右腕摄像头断开"),
    Voice("右腕恢复.wav", "右腕摄像头已恢复"),
    Voice("电池充电异常.wav", "电池充电异常"),
    Voice("外接电源故障.wav", "外接电源故障"),
    Voice("屏幕故障.wav", "屏幕故障"),
    Voice("设备自检通过.wav", "设备自检通过"),
    Voice("设备自检成功.wav", "设备自检成功"),
    Voice("风扇故障.wav", "风扇故障"),
    Voice("风扇恢复.wav", "风扇已恢复"),
    Voice("过温.wav", "过温"),
    Voice("温度恢复.wav", "温度已恢复"),
    Voice("关机成功.wav", "关机成功"),
    Voice("标0完成，请穿戴设备，录制准备动作.wav", "标0完成，请穿戴设备，录制准备动作"),
    # 标定流程按键反馈（F05-标定录制-按键播报补充需求）
    Voice("标定已开始，请保持静止.wav", "标定已开始，请保持静止"),
    Voice("标定已取消.wav", "标定已取消"),
    Voice("准备动作录制中.wav", "准备动作录制中"),
    Voice("标定完成.wav", "标定完成"),
    Voice("当前不能开始标定.wav", "当前不能开始标定"),
    Voice("未按要求执行准备动作录制.wav", "未按要求执行准备动作录制"),
    Voice("请先选择任务.wav", "请先选择任务"),
    Voice("设备正在收尾，请稍后.wav", "设备正在收尾，请稍后"),
)


async def synth_mp3(text: str, voice: str, rate: str, pitch: str) -> bytes:
    """合成 MP3；瞬时断连重试一次，再失败如实抛出。"""
    last_error: Exception | None = None
    for attempt in range(2):
        try:
            mp3 = bytearray()
            communicate = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
            async for chunk in communicate.stream():
                if chunk["type"] == "audio":
                    mp3.extend(chunk["data"])
            if not mp3:
                raise RuntimeError("未收到音频")
            return bytes(mp3)
        except Exception as exc:  # noqa: BLE001 - 统一重试/报错
            last_error = exc
            if attempt == 0:
                await asyncio.sleep(0.3)
    raise RuntimeError(f"{type(last_error).__name__}: {last_error}") from last_error


def write_wav(mp3: bytes, out_path: Path) -> dict[str, float]:
    decoded = miniaudio.decode(
        mp3,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=CHANNELS,
        sample_rate=SAMPLE_RATE,
    )
    pcm = array("h", decoded.samples)
    if sys.byteorder == "big":
        pcm.byteswap()

    peak = max((abs(s) for s in pcm), default=0)
    gain = min(TARGET_PEAK * 32767 / peak, MAX_GAIN) if peak else 1.0
    if gain > 1.0:
        def scale(sample: int) -> int:
            value = round(sample * gain)
            return -32768 if value < -32768 else 32767 if value > 32767 else value

        pcm = array("h", (scale(s) for s in pcm))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out_path), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm.tobytes())

    frames = len(pcm) // CHANNELS
    rms = (sum(s * s for s in pcm) / len(pcm)) ** 0.5 if pcm else 0.0
    final_peak = max((abs(s) for s in pcm), default=0)
    return {
        "duration": frames / SAMPLE_RATE,
        "peak": final_peak / 32767,
        "rms": rms / 32767,
        "gain": gain,
        "bytes": out_path.stat().st_size,
    }


def select(only: list[str]) -> list[Voice]:
    if not only:
        return list(VOICES)
    wanted = {Path(item).stem for item in only}
    picked = [v for v in VOICES if Path(v.filename).stem in wanted]
    missing = wanted - {Path(v.filename).stem for v in picked}
    if missing:
        sys.exit(f"清单里没有这些语音：{', '.join(sorted(missing))}")
    return picked


def verify(paths: list[Path]) -> int:
    """回读校验：必须是 44.1kHz / 双声道 / 16bit。"""
    bad = 0
    for path in paths:
        with wave.open(str(path), "rb") as wav:
            ok = (
                wav.getframerate() == SAMPLE_RATE
                and wav.getnchannels() == CHANNELS
                and wav.getsampwidth() == 2
            )
            if not ok:
                bad += 1
                print(f"  FAIL {path.name}: {wav.getframerate()}Hz "
                      f"{wav.getnchannels()}ch {wav.getsampwidth() * 8}bit")
    return bad


async def run(args: argparse.Namespace) -> int:
    voices = select(args.only)
    outputs: list[Path] = []
    for index, item in enumerate(voices, 1):
        out_path = OUT_DIR / item.filename
        print(f"[{index}/{len(voices)}] {item.filename} ← {item.text}", flush=True)
        mp3 = await synth_mp3(item.text, args.voice, args.rate, args.pitch)
        stats = write_wav(mp3, out_path)
        outputs.append(out_path)
        print(
            f"    OK {stats['duration']:.2f}s  峰值 {stats['peak']:.0%}  "
            f"RMS {stats['rms']:.0%}  x{stats['gain']:.2f}  "
            f"{stats['bytes'] // 1024}KB",
            flush=True,
        )

    bad = verify(outputs)
    if bad:
        print(f"校验失败：{bad}/{len(outputs)} 条格式不对")
        return 1
    print(f"完成：{len(outputs)} 条，全部 44100Hz/双声道/16bit")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 watchdog 播报语音")
    parser.add_argument("--voice", default=DEFAULT_VOICE, help="edge-tts 音色")
    parser.add_argument("--rate", default="+0%", help="语速，如 +10%% / -10%%")
    parser.add_argument("--pitch", default="+0Hz", help="音调，如 +2Hz / -2Hz")
    parser.add_argument("--only", nargs="+", default=[], help="只生成指定文件（可不带 .wav）")
    args = parser.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
