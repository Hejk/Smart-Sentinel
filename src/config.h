#pragma once
#include <Arduino.h>

// ============================================================
//  系统正式名称（2026-10 定稿）
//  智哨 Smart Sentinel —— 基于端云协同的农田生态防御系统
//  MQTT 设备标识仍沿用 sa/ir-node 前缀，这里只做展示名
// ============================================================
#define SYSTEM_NAME     "智哨 Smart Sentinel"
#define SYSTEM_TAGLINE  "基于端云协同的农田生态防御系统"

// ============================================================
//  静音模式（办公室调试专用）
//  1 = 屏蔽一切本地发声：开机两声、自检演示音、触发兜底提示音、
//      TF 卡 / 云端 wav 音频流 —— 全部数字静音（I2S 照常跑全 0 帧）
//  LED / RGB 爆闪 / 串口日志 / 事件记录不受影响，照样能验证逻辑
//  下班想听声音：改回 0 重新编译烧录即可
// ============================================================
#define QUIET_MODE      0      // ★ 音乐已打开：开机两声/自检提示音/驱赶音频全部出声

// ============================================================
//  引脚定义 —— 按工作台当前接线（原型 v2）
//  改线请同步改这里
// ============================================================

// —— 感应器：3 路 HC-SR501 人体红外（覆盖三个方向）
//    ★ HC-SR501 输出是"有人/有动物 = 高电平"，和之前的避障模块相反
#define PIN_PIR_1       15
#define PIN_PIR_2       16
#define PIN_PIR_3       4

// —— 音频：MAX98357 I2S 功放 + 3W 喇叭
#define PIN_I2S_BCLK    5
#define PIN_I2S_LRC     9
#define PIN_I2S_DIN     6
// 注：SD 脚悬空 = 默认使能；(L+R)/2 混合输出

// —— 音频存储：TF SD 卡（SPI）
// ★ 必须用 ESP32-S3 的「默认 SPI 脚位」：arduino-esp32 2.0.x 的 SD 库
//   会忽略自定义引脚，一律按板子默认脚（S3: SCK=12 MISO=13 MOSI=11）走总线。
//   所以接线也必须按这个接，否则驱动在错误的线上跟卡说话，永远挂载失败。
//   （血泪排查：裸探测用 SPI 实例自己的引脚是通的，就这个坑拖了很久）
#define PIN_SD_CS       10
#define PIN_SD_MOSI     11
#define PIN_SD_MISO     13          // ← 接 ESP32 GPIO13（模块 MISO）
#define PIN_SD_SCK      12          // ← 接 ESP32 GPIO12（模块 SCK）

// —— 视觉驱赶：WS2812 RGB 8 位
#define PIN_RGB_DIN     48
#define RGB_COUNT       8

// —— 状态指示
#define PIN_LED         47      // 高电平 = 亮

// —— AHT10 温湿度（I2C，本轮先挂上不使用）
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     7

// —— V2：AI 视觉主控（ESP32-S3_1）互联 UART1 ——
// 交叉接线：本板 TX17 → 视觉板 RX18；视觉板 TX17 → 本板 RX18
#define PIN_AI_TX            17
#define PIN_AI_RX            18
#define AI_UART_BAUD         115200
#define AI_RESULT_TIMEOUT_MS  5000UL   // 发 capture 后等识别结果的时限，超时自动放弃
#define AI_CONF_SPECIFIC      0.70f    // ≥它才切换专属音频；0.4~0.7 通用驱赶音继续响；<0.4 视觉板直接报 unknown

// —— 野猪专属联动（定稿：识别到野猪直接联动，不看置信度门槛）——
// 识别到野猪 → 播 wild_boar.wav + LED/RGB 同步红爆闪，直到报警窗口结束
#define BOAR_ALARM_MS   20000UL       // 报警总窗口（音频没放完会自动延长）
#define BOAR_FLASH_MS   90            // 爆闪节拍：LED 与 RGB 同步闪

// —— V2：OLED 状态屏（I2C 0x3C，与 AHT10 同总线）——
#define OLED_W           128
#define OLED_H           64          // 0.96寸 128x64；若屏是 128x32 改这里
#define OLED_ADDR        0x3C

// ============================================================
//  WiFi —— 调试阶段用；田野部署时换成 4G 模块（只换链路层）
// ============================================================
#define WIFI_SSID       "CU_h8Pk"   // ★ 联通光猫 2.4G 频段（ESP32 只支持 2.4G，_5G 那个连不上）
#define WIFI_PASS       "jb8pmtv3"

// ============================================================
//  MQTT broker
// ============================================================
#define MQTT_HOST       "broker.emqx.io"
#define MQTT_PORT       1883
#define MQTT_USER       ""
#define MQTT_PASS       ""
#define TOPIC_PREFIX    "sa/ir-node"

// ============================================================
//  运行参数
// ============================================================
#define WIFI_RETRY_MS   8000
#define MQTT_RETRY_MS   5000
#define HEARTBEAT_MS    10000
// ★ 注意：ESP32-S3 的变体头文件里已经定义了 RGB_BRIGHTNESS（给板载 WS2812 用），
//   所以这里换名字叫 STRIP_BRIGHTNESS，避免宏冲突。
//   也说明你这块板 GPIO48 上自带一颗 WS2812，它会和外部灯带的第 1 颗同步亮。
#define STRIP_BRIGHTNESS 80     // 0~255。8 颗全亮白光约 480mA，压低亮度省电

// ============================================================
//  深度睡眠（阶段 3b）
// ============================================================
#define SLEEP_ENABLE     0            // ★ 调试期临时关掉：串口调试中不想 15 秒就睡
                                           //   调试完部署前改回 1
#define AWAKE_WINDOW_MS  15000UL      // 醒来后留给"播放 + 联网上报"的窗口
#define POLICY_SYNC_SEC  (6ULL * 3600ULL)  // 无触发时每 6 小时醒来同步一次策略
#define PIR_WARMUP_MS    60000UL      // HC-SR501 冷启动预热（深睡唤醒不需要）

// ============================================================
//  事件日志 / 时间戳
//  日志写在 SD 卡根目录 /events.csv；时间靠 NTP，存 NVS 断电不丢
// ============================================================
#define TZ_INFO         "CST-8"              // 中国标准时间 UTC+8
#define NTP_SERVER_1    "ntp.aliyun.com"
#define NTP_SERVER_2    "pool.ntp.org"
#define EVENT_LOG_PATH  "/events.csv"

// —— 可选的低功耗门控（还没接线就保持 -1，代码会自动跳过）
//   MAX98357 的 SD 脚悬空时功放一直使能，静态吃几 mA；
//   接到 GPIO 上就能在睡觉前拉低关断。
#define PIN_AMP_SD       -1
//   WS2812 每颗即使关灯也在耗电（8 颗约 5~8mA），
//   用 MOSFET 门控它的 5V 是最彻底的省电手段。
#define PIN_RGB_PWR      -1