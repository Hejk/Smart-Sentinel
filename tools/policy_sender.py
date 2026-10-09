#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
红外唤醒语音节点 —— 云端策略模拟发送端

在电脑上跑，模拟"云端"通过 MQTT 给 ESP32 下发音频策略。
设备端订阅的主题形如:  sa/ir-node-<MAC后缀>/policy
MAC 后缀从设备串口日志里抄，例如：
    [启动] MQTT 主题（策略主题请发给它）:
           策略 ↓  sa/ir-node-EEFF/policy
那 --device 就填 EEFF。

用法
----
  安装依赖:      pip install paho-mqtt
  交互模式:      python policy_sender.py --device EEFF
  发一次预设:    python policy_sender.py --device EEFF --preset loud
  只听设备上报:  python policy_sender.py --device EEFF --watch
  生成测试音频:  python policy_sender.py --make-wav test.mp3.wav --seconds 3

  配合本地音频服务器（推荐，不依赖公网音频）:
     python policy_sender.py --make-wav test.wav --seconds 3
     python -m http.server 8000
     # 然后策略里 clip_url 填 http://<你电脑的IP>:8000/test.wav
"""

import argparse
import json
import math
import os
import struct
import sys
import threading
import time
import wave

DEFAULT_HOST = "broker.emqx.io"
DEFAULT_PORT = 1883
TOPIC_PREFIX = "sa/ir-node"

# ---------------------------------------------------------------- 预设策略
PRESETS = {
    "default": {
        "name": "default", "enabled": True, "mode": "trigger",
        "volume": 0.6, "tone_hz": 1200, "tone_ms": 250,
        "cooldown_ms": 3000, "hold_ms": 5000, "clip_url": "",
    },
    "loud": {
        "name": "loud", "enabled": True, "mode": "trigger",
        "volume": 1.0, "tone_hz": 880, "tone_ms": 500,
        "cooldown_ms": 2000, "hold_ms": 6000, "clip_url": "",
    },
    "quiet": {
        "name": "quiet", "enabled": True, "mode": "trigger",
        "volume": 0.25, "tone_hz": 1600, "tone_ms": 150,
        "cooldown_ms": 3000, "hold_ms": 3000, "clip_url": "",
    },
    "night": {          # 静音时段：只亮灯不发声
        "name": "night", "enabled": True, "mode": "mute",
        "volume": 0.0, "tone_hz": 1200, "tone_ms": 200,
        "cooldown_ms": 3000, "hold_ms": 2000, "clip_url": "",
    },
    "off": {            # 整机禁用
        "name": "off", "enabled": False, "mode": "trigger",
        "volume": 0.6, "tone_hz": 1200, "tone_ms": 250,
        "cooldown_ms": 3000, "hold_ms": 3000, "clip_url": "",
    },
    "fast": {           # 快速连触发场景：冷却很短
        "name": "fast", "enabled": True, "mode": "trigger",
        "volume": 0.7, "tone_hz": 2000, "tone_ms": 80,
        "cooldown_ms": 500, "hold_ms": 1000, "clip_url": "",
    },
    "cloud": {          # 演示第 2 阶段：触发时播放云端音频
        "name": "cloud", "enabled": True, "mode": "trigger",
        "volume": 0.8, "tone_hz": 1500, "tone_ms": 120,
        "cooldown_ms": 4000, "hold_ms": 8000,
        "clip_url": "http://192.168.1.100:8000/test.wav",
    },
}


# ---------------------------------------------------------------- 工具函数
def make_test_wav(path, seconds=3, rate=44100):
    """生成一段测试音频：两声上行音 + 一段 440Hz，够验证解码和出声"""
    n = int(rate * seconds)
    frames = bytearray()
    fade = 220
    # 前 1 秒：两个音；后 2 秒：持续 440Hz
    for i in range(n):
        if i < rate * 0.35:
            f, amp = 880.0, 0.8
            seg, seg_len = i, rate * 0.35
        elif i < rate * 0.7:
            f, amp = 1318.0, 0.8
            seg, seg_len = i - rate * 0.35, rate * 0.35
        else:
            f, amp = 440.0, 0.5
            seg, seg_len = i - rate * 0.7, n - rate * 0.7
        env = 1.0
        if seg < fade:
            env = seg / fade
        elif seg > seg_len - fade:
            env = max(0.0, (seg_len - seg) / fade)
        v = int(math.sin(2 * math.pi * f * i / rate) * env * amp * 32767)
        frames += struct.pack("<h", max(-32768, min(32767, v)))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    return path


def print_policy(p):
    print("  ┌── 即将下发的策略 ────────────────")
    for k in ("name", "enabled", "mode", "volume", "tone_hz", "tone_ms",
              "cooldown_ms", "hold_ms", "clip_url"):
        print(f"  │ {k:<12}: {p.get(k)}")
    print("  └──────────────────────────────────")


HELP = """
可用命令
  show                显示当前策略草稿
  set <字段> <值>     改字段 (如 set volume 0.8 / set mode mute)
  url <地址>          设置云端音频地址 (set clip_url 的快捷方式)
  url clear           清空云端音频地址
  preset <名字>       载入预设
  presets             列出所有预设
  send                发布当前策略（retained，设备上线就能收到）
  watch               开/关 设备上报的显示
  help                这份帮助
  quit / exit         退出
"""


# ---------------------------------------------------------------- 主程序
def main():
    ap = argparse.ArgumentParser(description="ESP32 红外唤醒节点 · 云端策略模拟发送端")
    ap.add_argument("--device", help="设备 MAC 后缀，如 EEFF（从串口日志抄）")
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"MQTT broker，默认 {DEFAULT_HOST}")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--user", default="")
    ap.add_argument("--passwd", default="", dest="passwd")
    ap.add_argument("--preset", choices=sorted(PRESETS), help="直接发送某个预设然后退出")
    ap.add_argument("--topic", help="直接指定策略主题（覆盖 --device）")
    ap.add_argument("--watch", action="store_true", help="只订阅设备上报，不发送")
    ap.add_argument("--no-retain", action="store_true", help="不发送 retained 消息")
    ap.add_argument("--make-wav", metavar="文件", help="生成测试音频 wav 后退出")
    ap.add_argument("--seconds", type=float, default=3.0, help="测试音频时长")
    args = ap.parse_args()

    # ---- 只生成测试音频
    if args.make_wav:
        p = make_test_wav(args.make_wav, args.seconds)
        print(f"✅ 已生成 {p}  ({os.path.getsize(p)} 字节)")
        print("   下一步：python -m http.server 8000")
        print("   然后把策略的 clip_url 设成 http://<你电脑IP>:8000/" + os.path.basename(p))
        return 0

    if not args.device and not args.topic:
        ap.error("必须给 --device <MAC后缀> 或 --topic <完整主题>")

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("缺少依赖，先装：pip install paho-mqtt", file=sys.stderr)
        return 2

    base = args.topic.rsplit("/", 1)[0] if args.topic else f"{TOPIC_PREFIX}-{args.device}"
    topic_policy = args.topic or f"{base}/policy"
    topic_event = f"{base}/event"
    topic_status = f"{base}/status"

    policy = dict(PRESETS["default"])
    show_reports = True

    # ---- MQTT
    cli = mqtt.Client(client_id=f"policy-sender-{int(time.time()) % 100000}")
    if args.user:
        cli.username_pw_set(args.user, args.passwd)
    connected = threading.Event()

    def on_connect(c, userdata, flags, rc):
        if rc == 0:
            connected.set()
            c.subscribe([(topic_event, 1), (topic_status, 1), (topic_policy, 1)])
            print(f"✅ 已连接 {args.host}:{args.port}")
            print(f"   策略主题 → {topic_policy}")
            print(f"   订阅上报 ← {topic_event}")
            print(f"            ← {topic_status}")
        else:
            print(f"❌ 连接失败 rc={rc}")

    def on_message(c, userdata, msg):
        if not show_reports:
            return
        tag = "设备事件" if msg.topic.endswith("/event") else \
              "设备状态" if msg.topic.endswith("/status") else "当前保留策略"
        payload = msg.payload.decode("utf-8", "replace")
        print(f"\n   ⬅ [{tag}] {payload}")

    cli.on_connect = on_connect
    cli.on_message = on_message
    try:
        cli.connect(args.host, args.port, 30)
    except Exception as e:
        print(f"❌ 连不上 broker: {e}", file=sys.stderr)
        return 1
    cli.loop_start()
    if not connected.wait(10):
        print("❌ 连接超时", file=sys.stderr)
        return 1

    def send(p):
        payload = json.dumps(p, ensure_ascii=False)
        cli.publish(topic_policy, payload, qos=1, retain=not args.no_retain)
        print(f"📤 已下发 → {topic_policy}")
        print(f"   {payload}")

    # ---- 单次发送预设
    if args.preset:
        send(PRESETS[args.preset])
        time.sleep(0.6)
        cli.loop_stop()
        return 0

    # ---- 只听
    if args.watch:
        print("👂 只监听设备上报，Ctrl+C 退出")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        cli.loop_stop()
        return 0

    # ---- 交互模式
    print(HELP)
    print_policy(policy)
    while True:
        try:
            line = input("policy> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        cmd, _, rest = line.partition(" ")
        cmd, rest = cmd.lower(), rest.strip()

        if cmd in ("quit", "exit", "q"):
            break
        elif cmd == "help":
            print(HELP)
        elif cmd == "show":
            print_policy(policy)
        elif cmd == "presets":
            for k, v in sorted(PRESETS.items()):
                print(f"  {k:<10} volume={v['volume']} mode={v['mode']} "
                      f"tone={v['tone_hz']}Hz/{v['tone_ms']}ms"
                      f"{'  clip_url=' + v['clip_url'] if v['clip_url'] else ''}")
        elif cmd == "preset":
            if rest in PRESETS:
                policy = dict(PRESETS[rest])
                print_policy(policy)
            else:
                print(f"没有预设 {rest}，用 presets 看列表")
        elif cmd == "set":
            k, _, v = rest.partition(" ")
            k, v = k.strip(), v.strip()
            if k not in policy:
                print(f"未知字段 {k}")
                continue
            try:
                if k in ("enabled",):
                    policy[k] = v.lower() in ("1", "true", "yes", "on")
                elif k in ("volume",):
                    policy[k] = max(0.0, min(1.0, float(v)))
                elif k in ("clip_url", "name", "mode"):
                    policy[k] = v
                else:
                    policy[k] = int(v)
                print(f"  {k} = {policy[k]}")
            except ValueError:
                print("值不合法")
        elif cmd == "url":
            policy["clip_url"] = "" if rest in ("clear", "") else rest
            print(f"  clip_url = {policy['clip_url'] or '(空)'}")
        elif cmd == "watch":
            show_reports = not show_reports
            print(f"  设备上报显示: {'开' if show_reports else '关'}")
        elif cmd == "send":
            send(policy)
        else:
            print(f"未知命令 {cmd}，敲 help")

    cli.loop_stop()
    print("👋 退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())