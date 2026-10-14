#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成占位驱赶音效（16kHz / 单声道 / 16bit WAV），直接放进 data/sounds/

这些是纯合成音，只用来把"本地音频播放"这条链路跑通。
真正驱赶要用真实录音（猛禽叫声、狗吠、人声吼叫…），替换方法见文件末尾。

用法:
    python tools/make_sound_pack.py                 # 默认输出到 data/sounds
    python tools/make_sound_pack.py --out data/sounds
"""

import argparse
import math
import os
import random
import struct
import wave

FADE = 200


def write_wav(path, samples, rate):
    buf = bytearray()
    for s in samples:
        v = int(max(-32768, min(32767, s)))
        buf += struct.pack("<h", v)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(buf))


def env(i, n, fade=FADE):
    if i < fade:
        return i / fade
    if i > n - fade:
        return max(0.0, (n - i) / fade)
    return 1.0


def pulse(rate, bursts=4, freq=2000.0, on_ms=110, off_ms=90, amp=0.85):
    """短促脉冲 · 通用警觉，最省电"""
    out = []
    for _ in range(bursts):
        n = int(rate * on_ms / 1000)
        for i in range(n):
            out.append(math.sin(2 * math.pi * freq * i / rate) * env(i, n) * amp * 32767)
        out.extend([0] * int(rate * off_ms / 1000))
    return out


def sweep(rate, f0=1800.0, f1=6500.0, ms=1200, repeats=2, amp=0.8):
    """高频扫频啸叫 · 鸟类对高频更敏感"""
    out = []
    for _ in range(repeats):
        n = int(rate * ms / 1000)
        phase = 0.0
        for i in range(n):
            phase += 2 * math.pi * (f0 + (f1 - f0) * i / n) / rate
            out.append(math.sin(phase) * env(i, n) * amp * 32767)
        out.extend([0] * int(rate * 0.15))
    return out


def roar(rate, ms=2000, amp=0.9):
    """低频轰鸣 · 对野猪/狍子这类大型动物更有效"""
    out = []
    n = int(rate * ms / 1000)
    phase = 0.0
    for i in range(n):
        f = 95 + 25 * math.sin(2 * math.pi * 0.7 * i / rate)
        phase += 2 * math.pi * f / rate
        s = math.sin(phase) + 0.35 * math.sin(2.3 * phase) + 0.15 * math.sin(4.1 * phase)
        out.append(s / 1.5 * env(i, n, 600) * amp * 32767)
    return out


def alarm(rate, pairs=4, fa=1500.0, fb=2500.0, seg_ms=170, amp=0.85):
    """双音警报 · 通用"""
    out = []
    for _ in range(pairs):
        for f in (fa, fb):
            n = int(rate * seg_ms / 1000)
            for i in range(n):
                out.append(math.sin(2 * math.pi * f * i / rate) * env(i, n, 60) * amp * 32767)
    return out


def hawk(rate, ms=800, amp=0.9):
    """尖啸 · 占位版猛禽叫声"""
    out = []
    n = int(rate * ms / 1000)
    phase = 0.0
    for i in range(n):
        phase += 2 * math.pi * (3200 - 2100 * (i / n) ** 1.6) / rate
        s = math.sin(phase) + 0.4 * math.sin(2 * phase)
        out.append(s / 1.4 * env(i, n, 120) * amp * 32767)
    return out


def boar(rate, grunts=7, amp=0.95):
    """wild_boar.wav
    """
    out = []
    rng = random.Random(27)
    for k in range(grunts):
        ms = 420 + 90 * k
        n = int(rate * ms / 1000)
        phase = 0.0
        base = 105 + 35 * (k % 3)
        for i in range(n):
            f = base * (1.0 + 0.16 * math.sin(2 * math.pi * 10.0 * i / rate))
            phase += 2 * math.pi * f / rate
            s = math.sin(phase) + 0.55 * math.sin(2.6 * phase) + 0.28 * math.sin(5.1 * phase)
            breath = (rng.random() - 0.5) * 0.20
            out.append((s / 1.83 + breath) * env(i, n, 400) * amp * 32767)
        out.extend([0.0] * int(rate * 0.16))
    n = int(rate * 850 / 1000)
    phase = 0.0
    for i in range(n):
        phase += 2 * math.pi * (650 + 950 * (i / n)) / rate
        s = math.sin(phase) + 0.4 * math.sin(2 * phase)
        out.append(s / 1.4 * env(i, n, 250) * amp * 32767)
    return out


PACK = [
    ("pulse_short.wav", pulse, "短促脉冲 · 通用警觉"),
    ("sweep_high.wav", sweep, "高频扫频 · 针对鸟类"),
    ("low_roar.wav", roar, "低频轰鸣 · 针对野猪/狍子"),
    ("alarm.wav", alarm, "双音警报 · 通用"),
    ("hawk_scream.wav", hawk, "尖啸 · 占位猛禽声"),
    ("wild_boar.wav", boar, "wild boar: low grunts + squeal"),
]

USAGE_NOTE = """
如何换成真实录音（推荐，驱赶效果比合成音好得多）
──────────────────────────────────────────────
1) 装上 ffmpeg，把下载/录制的音频转成固件要求的格式：
     ffmpeg -i 鹰叫.mp3 -ar 16000 -ac 1 -c:a pcm_s16le hawk_scream.wav
   ★ 必须是 16kHz / 单声道 / 16bit PCM WAV
     （固件支持 8k~96k、8/16bit，但 16k 单声道最省 Flash）

2) 把 wav 放进 code-project-1/data/sounds/

3) 重新生成文件系统镜像并烧录（在 STACK ANYWAY 里让我做）。

容量参考：16kHz 单声道 16bit ≈ 32KB/秒。
  · 3 秒音效 ≈ 96KB，3.5MB 分区能放约 35 条
  · 想让同样的分区装更多，用 8kHz 或缩短到 1.5 秒

用什么声音（按目标动物选）：
  · 野猪/狍子  → 狗吠、人声吼叫、低频轰鸣、鞭炮声
  · 乌鸦/喜鹊  → 猛禽（鹰/隼）叫声、同类惊叫、高频啸叫
  ★ 乌鸦和喜鹊是鸦科，聪明且习惯化快，务必多备几条随机轮换
  ★ 光靠声音会脱敏，建议同时加爆闪灯（鸟对强闪光也很敏感）
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/sounds", help="输出目录")
    ap.add_argument("--rate", type=int, default=16000)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    total = 0
    for name, fn, desc in PACK:
        path = os.path.join(args.out, name)
        write_wav(path, fn(float(args.rate)), args.rate)
        size = os.path.getsize(path)
        total += size
        print(f"  ✅ {name:<18} {size/1024:7.1f} KB   {desc}")
    print(f"\n共 {len(PACK)} 条，合计 {total/1024:.1f} KB → {args.out}")
    print(USAGE_NOTE)


if __name__ == "__main__":
    main()