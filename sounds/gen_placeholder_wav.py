#!/usr/bin/env python3
# Generate 8 placeholder WAVs for the watchdog voice slots.
#
# The real voice files ("左腕断开" etc.) will replace these later; the only
# requirement is identical naming and format: WAV / PCM16 / mono / 48000 Hz.
# Distinct alarm frequencies make it possible to tell which channel is alarming
# during bring-up (440/660/880/1100 Hz), recover tones use lower pitches.
#
#   usage: python3 gen_placeholder_wav.py   (writes into this directory)

import math
import os
import struct
import wave

SR = 48000
AMP = 0.35  # moderate loudness, no clipping

ALARM_FREQ = {
    "wrist_left.wav": 440,
    "wrist_right.wav": 660,
    "jhh02.wav": 880,
    "jhh04.wav": 1100,
}
RECOVER_FREQ = {
    "wrist_left_recover.wav": 330,
    "wrist_right_recover.wav": 392,
    "jhh02_recover.wav": 494,
    "jhh04_recover.wav": 587,
}


def tone(freq: float, seconds: float) -> list:
    n = int(SR * seconds)
    fade = int(SR * 0.005)  # 5 ms in/out to avoid clicks
    out = []
    for i in range(n):
        env = 1.0
        if i < fade:
            env = i / fade
        elif i >= n - fade:
            env = (n - 1 - i) / fade
        v = AMP * env * math.sin(2.0 * math.pi * freq * i / SR)
        out.append(int(max(-1.0, min(1.0, v)) * 32767))
    return out


def silence(seconds: float) -> list:
    return [0] * int(SR * seconds)


def alarm_samples(freq: float) -> list:
    # urgent triple beep: beep / gap / beep / gap / beep / tail
    s = []
    for k in range(3):
        s += tone(freq, 0.16)
        s += silence(0.09)
    s += silence(0.10)
    return s


def recover_samples(freq: float) -> list:
    # calm double beep at a lower pitch
    s = tone(freq, 0.30) + silence(0.12) + tone(freq, 0.30) + silence(0.08)
    return s


def write_wav(path: str, samples: list) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))
    print(f"wrote {path}: {len(samples)} samples ({len(samples)/SR:.2f}s)")


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    for name, freq in ALARM_FREQ.items():
        write_wav(os.path.join(here, name), alarm_samples(freq))
    for name, freq in RECOVER_FREQ.items():
        write_wav(os.path.join(here, name), recover_samples(freq))
    print("done: 8 placeholder WAVs")


if __name__ == "__main__":
    main()
