# tools/ —— 电脑端工具（不参与单片机编译）

这里的脚本跑在**你的电脑上**，不会被烧进 ESP32。

## policy_sender.py · 云端策略模拟发送端

模拟"云端"通过 MQTT 给设备下发音频策略，并实时显示设备上报的事件。

### 1. 装依赖

```bash
pip install paho-mqtt
```

### 2. 拿到设备的 MAC 后缀

设备开机后串口会打印：

```
[启动] MQTT 主题（策略主题请发给它）:
       策略 ↓  sa/ir-node-EEFF/policy
       事件 ↑  sa/ir-node-EEFF/event
       状态 ↑  sa/ir-node-EEFF/status
```

`EEFF` 就是 `--device` 要填的值（每块板子不一样）。

### 3. 交互下发

```bash
python policy_sender.py --device EEFF
```

进入后常用命令：

```
presets                看所有预设
preset loud            载入"大声"预设
set volume 0.8         改音量
set tone_hz 1800       改提示音频率
set hold_ms 4000       改点亮保持时长
url http://192.168.1.100:8000/test.wav   设云端音频地址
send                   下发（retained，设备重启也会收到）
```

一次性发送：

```bash
python policy_sender.py --device EEFF --preset night
```

只看设备上报：

```bash
python policy_sender.py --device EEFF --watch
```

### 4. 测"云端音频播放"（第 2 阶段）

不需要联网找音频，本地造一个就行：

```bash
python policy_sender.py --make-wav test.wav --seconds 3   # 生成 3 秒测试音频
python -m http.server 8000                                # 在 test.wav 所在目录起服务
```

然后 `clip_url` 填 `http://<你电脑的局域网IP>:8000/test.wav`。

查本机 IP：Windows `ipconfig`，Mac/Linux `ifconfig | grep 192.168`。
**必须用 `http://` 开头**（不是 https），MCU 端抓 https 要额外做 TLS，第 3 阶段再说。

### 5. 策略字段说明

| 字段 | 类型 | 含义 |
|---|---|---|
| name | string | 策略名，设备日志里会打印 |
| enabled | bool | `false` = 整机禁用，只记日志 |
| mode | string | `trigger` 正常 / `mute` 静音（只亮灯） |
| volume | 0.0~1.0 | 音量 |
| tone_hz | 100~8000 | 本地提示音频率 |
| tone_ms | 20~3000 | 本地提示音时长 |
| cooldown_ms | int | 两次触发的最小间隔，防连触 |
| hold_ms | int | 唤醒后 LED 点亮保持时长 |
| clip_url | string | 云端音频地址，非空则优先播放它 |

## 没有电脑端也可以直接测

不想装 Python 的话，用 MQTTX 或 EMQX 的网页客户端连 `broker.emqx.io:1883`，
往 `sa/ir-node-EEFF/policy` 发上面那段 JSON、勾选 retain 即可。