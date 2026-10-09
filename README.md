# 智哨 Smart Sentinel

> **基于端云协同的农田生态防御系统** · ESP32-S3 三分区驱赶节点

面向**野外农业场景**的驱赶动物节点：感应到动物 → 立刻播放本地音频 + 爆闪 → 联网上报 → 回去深度睡眠。

设计原则：**触发瞬间不联网**。声音和闪光全部来自本地，断网照常工作；联网只用来同步云端策略、更新音频、补报事件。

---

## 1. 硬件接线

引脚全部集中在 `src/config.h`，改线只改那一个文件。

| 模块 | 模块引脚 | ESP32-S3 | 说明 |
|---|---|---|---|
| HC-SR501 PIR ×3 | OUT | GPIO15 / 16 / 4 | 高电平=有活动，三方向覆盖 |
| | VCC / GND | **5V** / GND | ★ 不能接 3.3V，模块要求 4.5~20V |
| MAX98357 功放 | BCLK / LRC / DIN | GPIO5 / GPIO9 / GPIO6 | 标准 I2S |
| | Vin / GND | 3V3 / GND | SD 悬空=默认使能 |
| 3W 喇叭 | + / − | MAX98357 + / − | 单声道 BTL |
| WS2812 ×8 | DIN | GPIO48 | 视觉爆闪 |
| | 5V / GND | 5V / GND | 关灯态也耗电，建议 MOS 门控 |
| TF 卡模块 | CS / MOSI / MISO / SCK | GPIO10 / 11 / 12 / 13 | SPI，卡要 FAT32 |
| | VCC / GND | 3V3 / GND | 电源并 100µF |
| AHT10 | SDA / SCL | GPIO8 / GPIO7 | 预留未使用 |
| 状态 LED | + / − | GPIO47 / GND | 建议串 220Ω~1kΩ |
| 电池链路 | — | 18650 → TP4056 → 5V升压 → 5V0 | |

**接线注意**
- 三路 PIR 的 VCC 必须接 5V（接 3.3V 会不工作或乱触发），OUT 是 3.3V 电平，安全
- WS2812 用 3.3V 数据驱动 5V 灯带可能闪错色：灯带电源串二极管降到 4.3V，或加 74AHCT125
- 数据线串 300~500Ω、灯带电源并 1000µF，明显减少闪烁

---

## 2. 目录结构

```
code-project-1/
├─ src/                      固件源码（C++ / Arduino）
│   ├─ config.h              ★ 引脚、WiFi、MQTT、运行参数都在这里
│   ├─ main.cpp              主逻辑：唤醒 → 播放 → 联网 → 睡眠
│   ├─ pir_zones.h/.cpp      3 路 PIR 分区感应（上升沿触发 + 预热屏蔽）
│   ├─ audio_out.h/.cpp      I2S 输出（本地提示音 + 直接推 PCM）
│   ├─ wav_play.h/.cpp       通用 WAV(PCM) 播放器，本地和网络共用
│   ├─ http_audio.h/.cpp     HTTP/HTTPS 取流（含重定向）
│   ├─ sound_bank.h/.cpp     音频库：TF 卡优先、板载 Flash 兜底
│   ├─ led_strip.h/.cpp      WS2812 视觉驱赶
│   ├─ policy.h              云端策略结构 + JSON 解析
│   ├─ policy_store.h/.cpp   策略持久化到 NVS（深睡不丢）
│   └─ power.h/.cpp          唤醒源识别 + 深度睡眠
├─ data/sounds/              要打进文件系统的音频（5 条占位音效）
├─ tools/                    电脑端辅助脚本（Python，不烧进板子）
│   ├─ policy_sender.py      模拟云端下发策略 + 生成测试音频
│   └─ make_sound_pack.py    生成占位音效
├─ docs/                     中文文档（原画布上的"贴纸" + 方案图）
│   ├─ 00-到货前准备清单.txt
│   ├─ 01-BOM采购清单.txt
│   ├─ 02-田野硬件改造与BOM调整.txt
│   ├─ 03-接线对照与硬件风险.txt
│   ├─ 10~13-验证-阶段xxx.txt
│   └─ 图纸/                 可视化方案图（浏览器直接打开）
├─ partitions_8mb_ota.csv    分区表：双 OTA 槽 + 2.94MB LittleFS
└─ platformio.ini            构建配置
```

---

## 3. 文档索引

拿到这个项目后，建议按这个顺序看：

| 什么时候看 | 看哪份 |
|---|---|
| **现在就着** | `docs/00-到货前准备清单.txt` — 等货期间该准备什么 |
| 采购/烙铁前 | `docs/01-BOM采购清单.txt`、`docs/02-田野硬件改造与BOM调整.txt` |
| 插线前 | `docs/03-接线对照与硬件风险.txt` — 接线表 + 三个必须避的坑 |
| 烧录后验证 | `docs/10~13-验证-阶段xxx.txt` — 每一步的预期日志 + 出错怎么定位 |
| 想快速回忆 | 本文件（README） |
| 看方案图 | `docs/图纸/` 里两个 html，双击用浏览器打开 |

**图纸说明**
- `方案图-原型v2+深度睡眠.html` — 触发生命周期、功耗账、路线图
- `方案图-接线与采购规划.html` — 三个阶段的接线方式、采购建议

两份图纸都是单文件 HTML，不依赖任何外部资源，拷到哪里都能打开。

---

## 4. 编译与烧录

用 PlatformIO（VS Code 插件或 `pio` 命令行）。

```bash
pio run                 # 编译
pio run -t upload       # 烧录固件
pio run -t buildfs      # 生成文件系统镜像
pio run -t uploadfs     # 烧录文件系统镜像
```

⚠️ **要烧两个东西，不是一个**：

| 内容 | 说明 |
|---|---|
| 固件 `firmware.bin` | 程序本身 |
| 文件系统镜像 | 板载 Flash 里的音频（`data/` 目录打包） |

只烧固件不烧文件系统，开机会显示"音频库共 0 条"，然后一直走兜底提示音。

**音频放哪（两种都支持，自动选）**
1. **TF 卡（推荐）**：卡格式化成 **FAT32**，建 `/sounds` 目录放 wav
2. **板载 Flash**：把 wav 放进 `data/sounds/`，然后 `pio run -t buildfs && pio run -t uploadfs`

音频格式：**16kHz / 单声道 / 16bit PCM WAV**。转换：
```bash
ffmpeg -i 原始音频.mp3 -ar 16000 -ac 1 -c:a pcm_s16le 输出.wav
```

---

## 5. 串口命令（115200）

| 命令 | 作用 |
|---|---|
| `b` | 试听开机提示音 |
| `t` | 模拟触发（不分分区） |
| `g 1` | 模拟 1 号 PIR 触发（1~3），测分区曲库 |
| `w` | 测试 RGB 爆闪 |
| `p` / `s` | 打印当前策略 / 运行状态 |
| `l` / `n` | 列出音频库 / 播下一条 |
| `f <文件名>` | 播放指定文件 |
| `u <url>` | 试播云端音频（不经 MQTT，直接验证网络链路） |
| `x` | 停止播放 |
| `r` | 断开 MQTT 看重连 |
| `z` | **开关深度睡眠** |

★ **桌面调试先敲 `z` 关掉深度睡眠**，否则开机十几秒后它就睡了，睡着时串口没反应，容易误判成死机。只要有串口输入，睡眠会自动往后推 60 秒。

---

## 6. 云端策略（MQTT）

设备主题（`<MAC>` 是芯片 MAC 后四位，开机日志会打印）：

| 主题 | 方向 |
|---|---|
| `sa/ir-node-<MAC>/policy` | 云端 → 设备（retained） |
| `sa/ir-node-<MAC>/event` | 设备 → 云端（触发事件） |
| `sa/ir-node-<MAC>/status` | 设备 → 云端（在线状态） |

策略示例：

```json
{
  "name": "果园",
  "enabled": true,
  "mode": "trigger",
  "volume": 0.9,
  "cooldown_ms": 30000,
  "hold_ms": 4000,
  "sounds": ["alarm.wav", "pulse_short.wav"],
  "zones": {
    "1": ["hawk_scream.wav", "sweep_high.wav"],
    "2": ["low_roar.wav"],
    "3": "alarm.wav"
  },
  "pick": "random",
  "source": "local",
  "clip_url": "",
  "flash": { "enabled": true, "rgb": [255, 80, 0], "count": 3, "ms": 90 }
}
```

字段说明

| 字段 | 含义 |
|---|---|
| `enabled` | `false` = 整机禁用，只记日志 |
| `mode` | `trigger` 正常 / `mute` 静音（只亮灯不发声） |
| `volume` | 0.0~1.0 |
| `cooldown_ms` | 两次触发的最小间隔（深睡模式下同时是"冷却睡眠时长"） |
| `hold_ms` | 唤醒后 LED/RGB 保持时长 |
| `sounds` | 全局曲库（逗号分隔字符串或 JSON 数组都可以） |
| `zones` | 每路 PIR 各自的曲库，没写的分区用全局 |
| `pick` | `random` 随机不重复 / `round` 顺序轮换 |
| `source` | `local` 优先本地 / `cloud` 优先云端 URL |
| `clip_url` | 云端音频地址（WAV），`source=cloud` 时使用 |

测试工具：
```bash
pip install paho-mqtt
python tools/policy_sender.py --device <MAC后四位>      # 交互式下发
python tools/policy_sender.py --make-wav test.wav       # 生成测试音频
```

---

## 7. 深度睡眠工作方式

每次唤醒都是"重新开机"，完整流程：

```
深度睡眠（PIR 接 5V 常电，一直在监听）
   ↓ PIR 拉高，ext1 唤醒（同时得知是哪个分区）
立刻播放本地音频 + 爆闪    ← 毫秒级，不联网
   ↓
联网上报事件 + 同步策略（最多 15 秒）
   ↓
进入冷却睡眠：只开定时器、【关掉 PIR 唤醒】，睡 cooldown_ms
   ↓ 到点醒来
回到正常睡眠（PIR 唤醒 + 每 6 小时定时同步策略）
```

**为什么用"睡眠时长"当冷却**：深睡后没有时钟，"上次触发到现在多久"算不出来。而且这顺手解决了另一个坑 —— 动物赖着不走时 PIR 每 2.5 秒唤醒一次，会把电池吃干。

**策略持久化**：深睡清空 RAM，所以策略存进 NVS，收到就存一份，醒来先读回。

---

## 8. 当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 最小系统：PIR 唤醒 + LED + 提示音 + MQTT 策略 | 代码就绪 · **未实测** |
| 2 | 云端音频：策略 URL → HTTP 拉 WAV → I2S | 代码就绪 · **未实测** |
| 3a | 本地音频：TF 卡优先 / 板载 Flash 兜底、随机轮换 | 代码就绪 · **未实测** |
| 3b | 三分区感应 + 分区曲库 + WS2812 爆闪 | 代码就绪 · **未实测** |
| 3c | 深度睡眠 + 冷却睡眠 + 策略 NVS 持久化 | 代码就绪 · **未实测** |
| 4 | 功率收尾：灯带/功放门控、低 IQ 供电、太阳能 | 未开始 |
| 5 | 4G 联网（Air780E 封装成 Client） | 未开始 |
| 6 | 微波雷达补鸟 + OTA 远程升级 | 未开始 |

⚠️ **所有"代码就绪"只代表编译通过，尚未在实物上验证过任何一项。**

## 9. 已知限制

- 只支持 **WAV(PCM)** 8/16bit，**不支持 MP3**（主流音频库都要求 IDF5 新 I2S 驱动，与当前 Arduino core 2.0.17 不兼容，需要评估换 platform）
- 不支持 chunked 传输的 HTTP 音频
- HTTPS 用 `setInsecure()`，不校验证书
- 触发事件没上报成功就睡眠的话会丢（离线补报队列未做）
- **PIR 对鸟类基本无效**，防鸟需要再加微波雷达（未做）
- 三个待解决的常耗电项：WS2812 关灯态 5~8mA、MAX98357 SD 悬空几 mA、开发板 LDO 1~10mA。`config.h` 里的 `PIN_AMP_SD` / `PIN_RGB_PWR` 已预留门控开关（填 -1 表示未接线，代码自动跳过）