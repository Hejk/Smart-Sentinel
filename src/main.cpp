// ============================================================
//  ESP32-S3 红外唤醒驱赶节点  ·  原型 v2 + 深度睡眠
//
//  硬件：3 路 HC-SR501 PIR + MAX98357 + 3W 喇叭 + WS2812×8
//        + TF 卡（存音频）+ 18650 升压供电
//
//  阶段 1：PIR 触发 -> LED + 提示音 ; MQTT 收云端策略
//  阶段 2：策略 clip_url -> HTTP 拉 WAV -> I2S 播放
//  阶段 3a：本地音频优先（TF 卡 / 板载 Flash），断网照常工作
//  阶段 3b：深度睡眠 —— 唤醒即播放，播完联网上报，再回去睡
//  阶段 4：后台串口协议 —— 一行一条 JSON，日志回传/音频上传删除/状态/配置
//
//  ★ 深度睡眠下每次唤醒都是"重新开机"，所以整个流程变成：
//      PIR 拉高 -> 复位 -> setup 里读出唤醒脚 -> 立刻出声
//      -> 联网上报 + 同步策略（最多 AWAKE_WINDOW_MS）-> 回睡
//    策略存在 NVS 里，断电不丢。
//
//  串口命令：
//    b 试听      t 模拟触发      g <1~3> 模拟指定分区触发
//    w 爆闪      p 策略          s 状态          z 开关深度睡眠
//    l 音频库    n 下一条        f <文件名>       u <url> 试播云端
//    x 停止      r 断 MQTT       e 回放事件日志 /events.csv
//    q 查看离线事件队列（M1）
//    ★ 只要有串口输入，睡眠就会往后推，方便桌面调试
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <SD.h>
#include <time.h>

#include "config.h"
#include "pir_zones.h"
#include "audio_out.h"
#include "policy.h"
#include "wav_play.h"
#include "event_queue.h"
#include "http_audio.h"
#include "sound_bank.h"
#include "led_strip.h"
#include "power.h"
#include "policy_store.h"
#include "event_log.h"
#include "serial_cmd.h"
#include "ai_link.h"
#include "oled_ui.h"

PirZones     pir;
AudioOut     audio;
WavPlay      player(audio);
HttpAudio    http;
SoundBank    bank;
LedStrip     rgb;
Power        power;
PolicyStore  store;
EventLog     elog;                 // 事件日志：触发记录追加写 SD 卡 /events.csv
SerialCmd    serialCmd;            // 后台串口协议：一行一条 JSON 指令（7 条，见 docs）
AiLink       ai;                   // V2：AI 视觉板 UART1 链路（发抓拍命令 / 收识别结果）
OledUi       oled;                 // V2：OLED 状态屏（没接自动跳过）
Adafruit_AHTX0 aht;                 // AHT10 温湿度
bool          ahtOk = false;
WiFiClient   net;
PubSubClient mqtt(net);

File         curFile;
Client      *netClient = nullptr;

AudioPolicy  policy;
String       clientId, topicBase, topicPolicy, topicEvent, topicStatus;

bool     wifiUp        = false;
bool     waking        = false;
bool     playingNow    = false;
bool     didWakeEvent  = false;      // 有触发事件还没上报
bool     policySynced  = false;      // 本次醒来是否拿到策略
String   pendingZone;                // 待上报的分区名
String   playingWhat;
uint32_t lastWifiTry   = 0;
uint32_t lastMqttTry   = 0;
uint32_t lastHeartbeat = 0;
uint32_t lastTrig      = 0;
uint32_t activeUntil   = 0;
uint32_t awakeUntil    = 0;          // 本次清醒窗口的截止时刻
uint32_t mqttReadyAt   = 0;          // MQTT 连上的时刻
uint32_t lastSerialAct = 0;
bool     hadSerialAct  = false;
volatile uint8_t *g_wifiReasonPtr = nullptr;   // WiFi 断开原因码（事件回调只置值，主循环打印）
uint32_t lastWifiEvtPrint = 0;
bool     triggeredThisSession = false;   // 本次醒来发生过触发（决定睡不睡冷却）

// ------------------------------------------------------------ 外设电源门控
// 没接线（-1）时这些函数什么都不做，接上线就自动生效
static void ampEnable(bool on) {
#if (PIN_AMP_SD >= 0)
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

static void rgbPower(bool on) {
#if (PIN_RGB_PWR >= 0)
  pinMode(PIN_RGB_PWR, OUTPUT);
  digitalWrite(PIN_RGB_PWR, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

static void publishEvent(const char *event, const char *detail) {
  // M1：连着就发（offline:false），断着进队列，恢复后自动补报（offline:true）
  evq::submit(mqtt, topicEvent, policy.name, event, detail);
}

static void printStatus() {
  Serial.println("  ┌── 运行状态 ─────────────────────");
  Serial.printf("  │ 唤醒原因  : %s\n", power.causeText());
  Serial.printf("  │ 事件队列  : %u 条排队 / 累计挤掉 %lu 条\n",
                evq::pending(), evq::dropped());
  Serial.printf("  │ 运行时间  : %lus\n", (unsigned long)(millis() / 1000));
  Serial.printf("  │ 清醒窗口  : %lds 后睡觉\n",
                (long)((int32_t)(awakeUntil - millis()) / 1000));
  Serial.print("  │ PIR 电平  : ");
  for (uint8_t i = 0; i < PirZones::COUNT; i++)
    Serial.printf("%s=%s ", PirZones::name(i), pir.rawActive(i) ? "高" : "低");
  Serial.println();
  Serial.printf("  │ 预热      : %s\n", pir.warmupDone() ? "已完成" : "进行中(触发被忽略)");
  Serial.printf("  │ LED/RGB   : %s / %s\n",
                digitalRead(PIN_LED) ? "亮" : "灭", rgb.ready() ? "就绪" : "不可用");
  Serial.printf("  │ WiFi      : %s\n",
                strlen(WIFI_SSID) == 0 ? "离线模式(未配置)"
                                       : (wifiUp ? WiFi.localIP().toString().c_str() : "未连接"));
  Serial.printf("  │ MQTT      : %s   策略已同步:%s\n",
                mqtt.connected() ? "已连接" : "未连接", policySynced ? "是" : "否");
  Serial.printf("  │ 本地音频  : %d 条, 来源=%s\n", bank.count(), bank.backendName());
  Serial.printf("  │ AI视觉板  : %s   发%lu/收%lu\n",
                ai.statusText(), (unsigned long)ai.sentCount(), (unsigned long)ai.recvCount());
  Serial.printf("  │ OLED 屏   : %s\n", oled.ok() ? "就绪 0x3C" : "未接(跳过)");
  Serial.printf("  │ 事件日志  : %s   时间:%s   内存缓冲:%d 条\n",
                bank.sdMounted() ? "/events.csv (SD)" : "内存中(SD未挂载)",
                elog.timeValid() ? "已同步" : "未同步", elog.buffered());
  Serial.printf("  │ 播放中    : %s\n", playingNow ? playingWhat.c_str() : "否");
  Serial.printf("  │ 深度睡眠  : %s\n", power.sleepEnabled() ? "启用" : "关闭(调试)");
  Serial.println("  └──────────────────────────────────");
}

// ------------------------------------------------------------ 播放
static void stopPlayback() {
  if (playingNow) player.stop();
  if (curFile) curFile.close();
  http.close();
  netClient = nullptr;
  playingNow = false;
  playingWhat = "";
}

// ------------------------------------------------------------ 后台串口协议钩子
// serial_cmd 不直接摸播放器/传感器，通过这几个回调解耦
static void cbSerialActivity() {
  hadSerialAct = true;
  lastSerialAct = millis();       // canSleep() 靠这个推迟深度睡眠（60 秒静默后才允许睡）
}
static bool cbIsPlaying() { return playingNow; }
static void cbStopPlay()  { stopPlayback(); }
static bool cbReadClimate(float &t, float &h) {
  if (!ahtOk) return false;
  sensors_event_t hum, temp;
  if (!aht.getEvent(&hum, &temp)) return false;
  t = temp.temperature;
  h = hum.relative_humidity;
  return true;
}

static bool playLocal(int idx) {
  if (idx < 0 || !bank.mounted()) return false;
  curFile = bank.open(idx);
  if (!curFile) {
    Serial.printf("[本地] 打不开 %s\n", bank.name(idx).c_str());
    return false;
  }
  Serial.printf("[本地] 播放 %s (%u 字节)\n", bank.name(idx).c_str(), (unsigned)curFile.size());
  ampEnable(true);
  if (!player.begin(&curFile, policy.volume, nullptr)) {
    Serial.printf("[本地] 失败: %s\n", player.statusText());
    curFile.close();
    return false;
  }
  playingNow = true;
  playingWhat = bank.name(idx);
  return true;
}

static bool playUrl(const String &url) {
  if (!wifiUp) {
    Serial.println("[网络] WiFi 未连接，拉不到云端音频");
    return false;
  }
  netClient = http.open(url);
  if (!netClient) return false;
  ampEnable(true);
  if (!player.begin(netClient, policy.volume, netClient)) {
    http.close();
    netClient = nullptr;
    return false;
  }
  playingNow = true;
  playingWhat = url;
  return true;
}

// 写进 /events.csv 的分区名：PIR -> 1号/2号/3号，串口模拟 -> 串口
static String logZoneName(const char *source, int zone) {
  if (zone >= 0 && zone < (int)PirZones::COUNT) return String(PirZones::name((uint8_t)zone));
  if (source && strstr(source, "串口")) return String("串口");
  return String("----");
}

// ------------------------------------------------------------ V2：AI 识别结果落地
// 二段式驱赶：PIR 先响通用音效（不依赖视觉板），识别结果异步回来后：
//   ① 库里有专属音频 -> 打断通用音效，换专属音频（野猪叫对野猪更有效）
//   ② 专属灯色爆闪   ③ OLED 显示   ④ 上报云端（断网进 evq 队列）
static void onAiResult(const AiHit &hit) {
  Serial.printf("\n[AI] 识别结果: %s (%s)  置信度 %.0f%%\n", hit.en, hit.cn, hit.conf * 100.0f);

  const char *zone = pendingZone.length() ? pendingZone.c_str() : "AI";

  // ① 专属音频
  int idx = bank.find(hit.audio);
  bool switched = false;
  if (idx >= 0 && policy.enabled && policy.mode != "mute") {
    if (playingNow) stopPlayback();               // 打断通用音效，换专属音频
    switched = playLocal(idx);
    if (switched) {
      elog.log(zone, hit.audio, "ai-start");
      activeUntil = millis() + policy.holdMs;     // 保持时间重算
      waking = true;
      digitalWrite(PIN_LED, HIGH);
    }
  }
  if (idx < 0) {
    Serial.printf("[AI] 音频库没有 %s —— 传一个进 /sounds 就能自动命中\n", hit.audio);
    elog.log(zone, hit.audio, "ai-nomatch");
  } else if (!switched) {
    elog.log(zone, hit.audio, "ai-skip");         // 静音策略或播放失败，只记录
  }

  // ② 专属灯色
  if (rgb.ready()) {
    if (policy.flash)
      rgb.strobe(hit.r, hit.g, hit.b, policy.flashCount, policy.flashMs, policy.flashMs);
    else
      rgb.blip(hit.r, hit.g, hit.b, 120);
  }

  // ③ OLED（没接自动跳过）
  if (oled.ok()) oled.aiResult(hit.en, hit.conf);

  // ④ 云端上报（evq：断网自动排队，恢复补报）
  char detail[160];
  snprintf(detail, sizeof(detail),
           "animal=%s,label_cn=%s,confidence=%.2f,audio=%s,played=%s",
           hit.en, hit.cn, hit.conf, hit.audio, switched ? "true" : "false");
  publishEvent("ai_result", detail);
}

// ------------------------------------------------------------ 唤醒 -> 立刻出声
// 顺序很重要：
//   1) 先把声音和闪光放出来（本地，毫秒级）
//   2) 再谈联网。田野里断网是常态，不能因为连不上网就让动物跑掉。
static void doWake(const char *source, int zone) {
  uint32_t now = millis();
  waking      = true;
  activeUntil = now + policy.holdMs;
  digitalWrite(PIN_LED, HIGH);

  const char *zoneName = (zone >= 0 && zone < (int)PirZones::COUNT)
                           ? PirZones::name((uint8_t)zone) : "无分区";
  String logZone = logZoneName(source, zone);      // 写进 /events.csv 的分区名

  // V2：让视觉板同步抓拍识别（结果异步回来，在 onAiResult 里接；
  //     视觉板不在线也不影响下面的通用驱赶流程，超时自动放弃）
  ai.requestCapture(zoneName);

  Serial.printf("\n[唤醒] 来源=%s  分区=%s  t=%lums  保持 %lums\n",
                source, zoneName, (unsigned long)now, (unsigned long)policy.holdMs);
  printPolicy(policy);

  if (!policy.enabled || policy.mode == "mute") {
    Serial.println("[音频] 策略为静音/禁用 -> 只亮灯不发声");
    elog.log(logZone.c_str(), "----", "muted");
    didWakeEvent = true;
    triggeredThisSession = true;
    pendingZone = "静音";
    return;
  }

  // 视觉驱赶先上：闪光比声音更不容易被动物习惯
  if (policy.flash && rgb.ready()) {
    Serial.printf("[RGB] 爆闪 %u 次 RGB(%u,%u,%u) 每下 %ums\n",
                  policy.flashCount, policy.flashR, policy.flashG, policy.flashB, policy.flashMs);
    rgb.strobe(policy.flashR, policy.flashG, policy.flashB,
               policy.flashCount, policy.flashMs, policy.flashMs);
  }

  // 该分区有专属曲库就用自己的，否则用全局
  String list = policy.sounds;
  if (zone >= 0 && zone < (int)PirZones::COUNT && policy.zoneSounds[zone].length()) {
    list = policy.zoneSounds[zone];
    Serial.printf("[音频] 使用 %s 专属曲库\n", zoneName);
  }

  // ★ 日志写在“播放开始前”，SD 写入的零点几秒卡顿不会打断声音
  bool played = false;
  if (policy.source == "cloud" && policy.clipUrl.length()) {
    elog.log(logZone.c_str(), policy.clipUrl.c_str(), "cloud-start");
    played = playUrl(policy.clipUrl);
  } else {
    int idx = bank.pick(list, policy.pick != "round");
    if (idx >= 0) {
      Serial.printf("[音频] 本地库选中 %d 号\n", idx);
      elog.log(logZone.c_str(), bank.name(idx).c_str(), "start");
      played = playLocal(idx);
    }
    if (!played && policy.clipUrl.length()) {
      Serial.println("[音频] 本地不可用，改试云端音频");
      elog.log(logZone.c_str(), policy.clipUrl.c_str(), "cloud-start");
      played = playUrl(policy.clipUrl);
    }
  }
  if (!played) {
    Serial.printf("[音频] 兜底：本地提示音 %uHz %ums\n", policy.toneHz, policy.toneMs);
    elog.log(logZone.c_str(), "----", "tone-fallback");
    ampEnable(true);
    audio.tone(policy.toneHz, policy.toneMs);
  }
  didWakeEvent = true;
  triggeredThisSession = true;
  pendingZone = zoneName;
  Serial.printf("[上报] 事件已入队，等 MQTT 连上就发（分区=%s）\n", pendingZone.c_str());
}

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  Serial.printf("\n[MQTT] 收到 %s (%u 字节)\n", topic, len);
  static char buf[1024];
  unsigned int n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;
  Serial.printf("       原文: %s\n", buf);

  if (parsePolicyOrUrl(buf, policy)) {
    audio.setVolume(policy.volume);
    Serial.println("[策略] 解析成功，已生效 ↓");
    printPolicy(policy);
    store.save(String(buf));          // 存进 NVS，深睡醒来不会丢
    policySynced = true;
    if (rgb.ready()) rgb.blip(0, 200, 0, 80);
    digitalWrite(PIN_LED, LOW);
    delay(40);
    digitalWrite(PIN_LED, waking ? HIGH : LOW);
    publishEvent("policy_applied", policy.name.c_str());
  } else {
    Serial.println("[策略] 解析失败，保持原策略");
  }
}

// ------------------------------------------------------------ 连接维护
static bool wifiDiagDone = false;
static void wifiDiagScan() {
  Serial.println("[WiFi] ▼ 连接不顺，扫描周围 2.4G 网络（诊断）：");
  // ★ STA 正在关联中时 scanNetworks() 会直接失败(-2)。
  //   先断开关联再扫，否则看到的“扫描失败”是假象。
  WiFi.disconnect();
  delay(300);
  for (int round = 1; round <= 2; round++) {        // 连扫两轮，防单次抽风
    int n = WiFi.scanNetworks();
    Serial.printf("      第%d轮扫描: %d 个网络%s\n", round, n,
                  n < 0 ? "（扫描仍失败：射频侧异常，重启板子试试）" : "");
    if (n <= 0) { delay(400); continue; }
    bool found = false;
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      bool hit = (ssid == WIFI_SSID);
      if (hit) found = true;
      Serial.printf("      %-24s RSSI=%3d dBm  信道=%2d  %s%s\n",
                    ssid.length() ? ssid.c_str() : "(隐藏网络)",
                    WiFi.RSSI(i), WiFi.channel(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "开放 " : "加密 ",
                    hit ? " ← 目标" : "");
    }
    if (!found)
      Serial.printf("      ★ 目标 \"%s\" 不在扫描结果里\n", WIFI_SSID);
    else
      Serial.println("      目标网络可见 → 连不上就看断开原因码");
    WiFi.scanDelete();
    break;                                             // 拿到结果就不再扫第二轮
  }
}

static void loopWifi() {
  if (!strlen(WIFI_SSID)) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      wifiUp = true;
      Serial.printf("[WiFi] 已连接  IP=%s  RSSI=%d  (耗时 %lums)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)millis());
      static bool ntpStarted = false;          // 首次连上才启动 SNTP（每次开机一次）
      if (!ntpStarted) {
        ntpStarted = true;
        configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
        Serial.printf("[NTP] 时间同步已启动 (时区 %s)\n", TZ_INFO);
      }
    }
    return;
  }
  wifiUp = false;
  if (millis() - lastWifiTry < WIFI_RETRY_MS) return;
  lastWifiTry = millis();
  if (!wifiDiagDone && millis() > 20000) {   // 起机 20 秒还没连上，先扫一次
    wifiDiagDone = true;
    wifiDiagScan();
  }
  Serial.printf("[WiFi] 连接 \"%s\" ...  (状态=%d)\n", WIFI_SSID, WiFi.status());
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void loopMqtt() {
  if (!wifiUp) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();

  Serial.printf("[MQTT] 连接 %s:%d ...\n", MQTT_HOST, MQTT_PORT);
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                      topicStatus.c_str(), 1, true, "{\"online\":false}");
  } else {
    ok = mqtt.connect(clientId.c_str(), topicStatus.c_str(), 1, true, "{\"online\":false}");
  }

  if (ok) {
    mqttReadyAt = millis();
    Serial.printf("[MQTT] 已连接 OK (耗时 %lums)\n", (unsigned long)millis());
    mqtt.subscribe(topicPolicy.c_str(), 1);
    Serial.printf("[MQTT] 已订阅策略主题: %s\n", topicPolicy.c_str());
    char st[200];
    snprintf(st, sizeof(st),
             "{\"online\":true,\"ip\":\"%s\",\"policy\":\"%s\",\"sounds\":%d,\"wake\":\"%s\"}",
             WiFi.localIP().toString().c_str(), policy.name.c_str(), bank.count(),
             power.causeText());
    mqtt.publish(topicStatus.c_str(), st, true);
  } else {
    Serial.printf("[MQTT] 连接失败 rc=%d，%lums 后重试\n",
                  mqtt.state(), (unsigned long)MQTT_RETRY_MS);
  }
}

// ------------------------------------------------------------ 串口命令
static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    // JSON 指令（后台串口协议）优先：'{' 开头到行尾都归它管
    if (serialCmd.charge(c)) continue;
    hadSerialAct = true;
    lastSerialAct = millis();
    switch (c) {
      case 'b':
        Serial.println("[CMD] 试听提示音");
        ampEnable(true);
        audio.chime();
        ampEnable(false);
        break;
      case 't':
        Serial.println("[CMD] 模拟触发（不分分区）");
        doWake("串口模拟", -1);
        break;
      case 'g': {
        String a = Serial.readStringUntil('\n');
        a.trim();
        int z = a.toInt();
        if (z < 1 || z > (int)PirZones::COUNT) {
          Serial.printf("[CMD] 用法: g 1   (模拟 1~%d 号 PIR 触发)\n", PirZones::COUNT);
          break;
        }
        Serial.printf("[CMD] 模拟 %s 触发\n", PirZones::name((uint8_t)(z - 1)));
        doWake("串口模拟", z - 1);
        break;
      }
      case 'w':
        if (rgb.ready()) {
          Serial.println("[CMD] 测试爆闪");
          rgb.strobe(policy.flashR, policy.flashG, policy.flashB,
                     policy.flashCount, policy.flashMs, policy.flashMs);
        } else {
          Serial.println("[CMD] RGB 灯带不可用");
        }
        break;
      case 'p': printPolicy(policy); break;
      case 's': printStatus(); break;
      case 'l': bank.printAll(); break;
      case 'z':
        power.setSleepEnabled(!power.sleepEnabled());
        Serial.printf("[CMD] 深度睡眠 %s\n", power.sleepEnabled() ? "已启用" : "已关闭");
        if (power.sleepEnabled()) {
          awakeUntil = millis() + AWAKE_WINDOW_MS;
        } else {
          awakeUntil = millis() + 3600000UL;   // 调试期间别再睡
        }
        break;
      case 'n': {
        int idx = bank.pick(policy.sounds, policy.pick != "round");
        Serial.printf("[CMD] 下一条 -> %d 号\n", idx);
        if (!playLocal(idx)) Serial.println("[CMD] 没有可播的音频");
        break;
      }
      case 'x':
        if (playingNow) {
          stopPlayback();
          Serial.println("[CMD] 已停止播放");
        }
        break;
      case 'r':
        if (mqtt.connected()) {
          mqtt.disconnect();
          Serial.println("[CMD] 已断开 MQTT");
        }
        break;
      case 'e':
        Serial.println("[CMD] 回放事件日志");
        elog.dumpAll();
        break;
      case 'q':
        Serial.println("[CMD] 查看离线事件队列");
        evq::dump();
        break;
      case 'c':
        Serial.println("[CMD] 手动向视觉板发 {\"cmd\":\"capture\"}（台架联调用）");
        ai.requestCapture("串口");
        break;
      case 'f':
      case 'u': {
        String arg = Serial.readStringUntil('\n');
        arg.trim();
        if (!arg.length()) {
          Serial.println(c == 'f' ? "[CMD] 用法: f hawk_scream.wav"
                                  : "[CMD] 用法: u http://192.168.1.10:8000/test.wav");
          break;
        }
        if (c == 'f') {
          int idx = bank.find(arg);
          if (idx < 0) {
            Serial.printf("[CMD] 存储里没有 %s\n", arg.c_str());
            bank.printAll();
            break;
          }
          playLocal(idx);
        } else {
          Serial.printf("[CMD] 试播云端音频: %s\n", arg.c_str());
          if (!playUrl(arg)) Serial.println("[CMD] 试播失败，看上面的原因");
        }
        break;
      }
    }
  }
}

// ------------------------------------------------------------ 该不该睡
static bool canSleep() {
  if (!power.sleepEnabled()) return false;
  if (playingNow) return false;                                     // 还在放音
  if (ai.awaiting()) return false;                                  // 还在等视觉板识别结果
  if (hadSerialAct && (millis() - lastSerialAct) < 60000UL) return false;  // 有人在调
  if (millis() < 3000UL) return false;                              // 至少醒 3 秒，让日志出来
  if (millis() >= awakeUntil) return true;                          // 窗口到了，不管有没有连上网
  // 事件报完 + 队列倒空 + MQTT 稳了一小会儿 -> 可以提前睡，省电
  if (!didWakeEvent && evq::pending() == 0 && mqtt.connected() && mqttReadyAt &&
      (millis() - mqttReadyAt > 1500UL)) return true;
  return false;
}

static void goToSleep() {
  Serial.println("\n[睡眠] 准备入睡 ...");
  stopPlayback();
  digitalWrite(PIN_LED, LOW);
  if (rgb.ready()) rgb.off();
  rgbPower(false);
  ampEnable(false);
  audio.flush();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);

  // 触发过的话，进冷却睡（关掉 PIR 唤醒，防动物赖着不走把电耗光）
  if (triggeredThisSession && policy.cooldownMs > 0) {
    uint64_t sec = (policy.cooldownMs + 999ULL) / 1000ULL;
    if (sec > 3600ULL) sec = 3600ULL;
    power.enterCooldownSleep(sec);        // 不返回
  }
  power.enterDeepSleep(POLICY_SYNC_SEC);  // 不返回
}

// ------------------------------------------------------------
static int zoneFromMask(uint32_t mask) {
  for (uint8_t i = 0; i < PirZones::COUNT; i++)
    if (mask & (uint32_t)(1ULL << PirZones::pin(i))) return (int)i;
  return -1;
}

void setup() {
  Serial.begin(115200);
  delay(30);                 // 深睡唤醒要快，不能像平常那样等 400ms

  power.begin();
  evq::begin();              // 离线事件队列清零（M1）

  if (power.cause() == WAKE_COLD) {
    Serial.println();
    Serial.println("============================================");
    Serial.printf(" %s\n", SYSTEM_NAME);
    Serial.printf(" %s\n", SYSTEM_TAGLINE);
    Serial.println(" ESP32-S3 三分区驱赶节点 · v2 + 深度睡眠");
    Serial.printf(" PIR -> GPIO%d/%d/%d   LED -> GPIO%d   RGB -> GPIO%d\n",
                  PIN_PIR_1, PIN_PIR_2, PIN_PIR_3, PIN_LED, PIN_RGB_DIN);
    Serial.printf(" I2S -> GPIO%d/%d/%d    SD -> CS%d SCK%d MISO%d MOSI%d\n",
                  PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN,
                  PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);
    Serial.printf(" AI  -> UART1 TX%d/RX%d (视觉板)   OLED -> 0x%02X@SDA%d/SCL%d\n",
                  PIN_AI_TX, PIN_AI_RX, OLED_ADDR, PIN_I2C_SDA, PIN_I2C_SCL);
    Serial.println("============================================");
  }
  power.printWakeInfo();

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  // 开机自检信号：LED 连闪 3 下 = 固件活着（不依赖串口，肉眼可见）
  if (power.cause() == WAKE_COLD) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_LED, HIGH); delay(120);
      digitalWrite(PIN_LED, LOW);  delay(120);
    }
  }
  ampEnable(false);
  rgbPower(true);

  // 冷却结束的醒来：什么都不用干，直接回去正常睡
  if (power.cause() == WAKE_COOLDOWN) {
    Serial.println("[启动] 冷却期结束，直接回正常睡眠");
    power.enterDeepSleep(POLICY_SYNC_SEC);     // 不返回
  }

  // 1) I2S 音频
  if (!audio.begin()) Serial.println("[启动] I2S 初始化失败，不会有声音");
  audio.setVolume(policy.volume);

  // 2) 本地音频库（TF 卡优先，板载 Flash 兜底）
  bank.begin();

  // 2.5) 事件日志：SD 挂上了直写 /events.csv，没挂上先攒在内存，卡挂上自动补写
  elog.begin(bank.sdMounted() ? (fs::FS *)&SD : nullptr, EVENT_LOG_PATH);
  if (power.cause() == WAKE_COLD) elog.log("boot", "----", "boot");

  // 3) PIR：冷启动才需要预热，深睡唤醒立即可用
  pir.begin(power.cause() == WAKE_COLD);

  // 4) RGB 灯带
  rgb.begin();

  // 4.5) AHT10 温湿度（I2C：SDA=8 SCL=7，地址 0x38）
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  ahtOk = aht.begin(&Wire);
  if (ahtOk) {
    Serial.println("[AHT10] ✓ 温湿度传感器就绪  SDA=GPIO8 SCL=GPIO7");
  } else {
    Serial.println("[AHT10] ✗ 未找到（0x38 无应答）—— 检查 SDA/SCL/VCC/GND 四根线");
  }

  // 4.7) V2：AI 视觉板链路 + OLED 状态屏（都没接好也不影响 v1 功能）
  ai.begin();
  oled.begin();
  ai.ping();          // 链路自检：视觉板在线会回 pong

  // 5) 先把上次存的策略读回来（深睡会把 RAM 清空）
  store.begin();
  String saved;
  if (store.load(saved) && parsePolicy(saved.c_str(), policy)) {
    Serial.println("[策略] 已从 NVS 恢复上次云端策略");
    audio.setVolume(policy.volume);
  }

  // 6) 网络
  WiFi.mode(WIFI_STA);            // ★ 先开 STA 再取 MAC，否则 MAC 读出来全 0
  WiFi.setSleep(false);
  // ★ WiFi 断开原因码：只记在 volatile 变量里，主循环里再打印
  //   （在事件回调里直接 Serial.printf 有死锁风险）
  static volatile uint8_t s_wifiReason = 0;
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    s_wifiReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  g_wifiReasonPtr = &s_wifiReason;
  {
    uint8_t b[6];
    esp_read_mac(b, ESP_MAC_WIFI_STA);        // 从 eFuse 直读，任何时候都有效
    char s[7]; snprintf(s, sizeof(s), "%02X%02X", b[4], b[5]);
    clientId = String("esp32-ir-") + s;
    topicBase = String(TOPIC_PREFIX) + "-" + s;
  }
  topicPolicy = topicBase + "/policy";
  topicEvent  = topicBase + "/event";
  topicStatus = topicBase + "/status";

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  Serial.printf("[启动] 策略主题: %s\n", topicPolicy.c_str());
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    lastWifiTry = millis();
  }

  awakeUntil = millis() + AWAKE_WINDOW_MS;

  // 6.5) 后台串口协议（JSON 7 指令）
  {
    SerialCmd::Hooks hk;
    hk.activity    = cbSerialActivity;
    hk.isPlaying   = cbIsPlaying;
    hk.stopPlayback = cbStopPlay;
    hk.readClimate = cbReadClimate;
    serialCmd.begin(hk);
  }

  printPolicy(policy);

  // 7) 按唤醒原因干活
  if (power.cause() == WAKE_PIR) {
    int zone = zoneFromMask(power.pirWakeMask());
    doWake("PIR 深睡唤醒", zone);          // 立刻出声，不等联网
    lastTrig = millis();
  } else if (power.cause() == WAKE_TIMER) {
    Serial.println("[启动] 定时唤醒：只同步策略，不发声");
    elog.log("boot", "----", "timer-wake");   // 田野里看最后一行就知道节点还活着
  } else {
    Serial.println("[启动] 冷启动：响两声开机提示音");
    ampEnable(true);
    audio.chime();
    ampEnable(false);
  }
  Serial.println("[启动] 完成。命令: b试听 c抓拍 t触发 g<1-3>分区 w爆闪 z睡眠开关 p/s/l/e/n/f/u/x/r");
  Serial.println("[启动] 后台协议: {\"cmd\":\"get_status\"} 等 7 条 JSON 指令，见 docs/20-后台串口协议.md");
}

void loop() {
  handleSerial();

  // —— V2：AI 视觉板识别结果（异步，二段式驱动的第二段）——
  AiHit aiHit;
  if (ai.poll(aiHit)) onAiResult(aiHit);
  oled.tick();

  loopWifi();
  loopMqtt();

  // —— WiFi 断开原因码：主循环里打印（安全）——
  if (g_wifiReasonPtr && *g_wifiReasonPtr) {
    uint8_t r = *g_wifiReasonPtr;
    *g_wifiReasonPtr = 0;
    const char *why = "其它";
    if (r == 201)   why = "没找到热点(热点关了/5G专属SSID)";
    else if (r == 15) why = "密码错误";
    else if (r == 2)  why = "认证超时";
    else if (r == 202) why = "认证失败";
    else if (r == 36) why = "已断开(常见于热点关闭/超时)";
    if (millis() - lastWifiEvtPrint > 2000UL) {   // 防刷屏：2秒最多一条
      lastWifiEvtPrint = millis();
      Serial.printf("[WiFi事件] 断开 原因码=%d (%s)\n", r, why);
    }
  }

  // —— 心跳：每 10 秒报一次平安（调试模式专用，确认板子没死）——
  static uint32_t lastBeat = 0;
  if ((SLEEP_ENABLE == 0) && millis() - lastBeat > 10000UL) {
    lastBeat = millis();
    char ahtBuf[40] = " AHT10=未接";
    if (ahtOk) {
      sensors_event_t hum, temp;
      if (aht.getEvent(&hum, &temp))
        snprintf(ahtBuf, sizeof(ahtBuf), "  %.1f°C  %.0f%%RH",
                 temp.temperature, hum.relative_humidity);
      else
        snprintf(ahtBuf, sizeof(ahtBuf), "  AHT10=读数失败");
    }
    Serial.printf("[心跳] 运行中 %lus  WiFi=%s  SD=%s  MQTT=%s%s\n",
                  (unsigned long)(millis() / 1000),
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "未连接",
                  bank.sdMounted() ? "已挂载" : "未挂载",
                  mqtt.connected() ? "在线" : "离线", ahtBuf);
  }

  // —— TF 卡挂载重试：失败后每 10 秒自动再试（调试模式下无限重试）——
  //    现在插紧卡 / 换对电源脚，不用重启，日志实时告诉你挂上没
  static uint32_t lastSdRetry = 0;
  static uint8_t  sdRetries   = 0;
  if (!bank.sdMounted() && (SLEEP_ENABLE == 0) &&
      (millis() - lastSdRetry > 10000UL)) {
    lastSdRetry = millis();
    if (bank.retrySD()) {
      elog.setFS(&SD);            // 卡挂上了：事件日志切到 SD，内存缓冲自动补写
    } else if (++sdRetries % 3 == 0) {
      Serial.println("[SD] 提示：卡要按到底听咔哒声；模块上若焊有 AMS1117 稳压芯片，VCC 必须接 5V 而不是 3V3");
    }
  }

  // —— 开机自检演示（仅调试模式）：上电 8 秒后自动跑一遍 ——
  // 不需要串口输入：LED + 板载RGB爆闪 + 提示音，用眼睛确认硬件活着
  static bool selfTestDone = false;
  if (!selfTestDone && (SLEEP_ENABLE == 0) && power.cause() == WAKE_COLD &&
      millis() > 8000UL) {
    selfTestDone = true;
    Serial.println("[自检] ▶ 自动演示开始：LED亮 -> RGB爆闪3下 -> 提示音（喇叭未焊，无声属正常）");
    digitalWrite(PIN_LED, HIGH);                 // LED 灯珠 GPIO47
    if (rgb.ready())
      rgb.strobe(policy.flashR, policy.flashG, policy.flashB,
                 policy.flashCount, policy.flashMs, policy.flashMs);
    ampEnable(true);
    audio.chime();                                // MAX98357 两声提示音
    ampEnable(false);
    digitalWrite(PIN_LED, LOW);
    Serial.println("[自检] ✓ 完成。串口命令仍可用: b试听 t触发 w爆闪 s状态 l音频库");
  }

  // —— 离线事件补报：MQTT 连着就按顺序慢慢倒（150ms 一条，别冲着代理）——
  static uint32_t lastDrain = 0;
  if (mqtt.connected() && evq::pending() && millis() - lastDrain > 150) {
    lastDrain = millis();
    evq::drainOne(mqtt, topicEvent, policy.name);
  }

  // 触发事件：直接交队列（连着就发，断着就排队，恢复自动补报）
  if (didWakeEvent) {
    publishEvent("pir_trigger", pendingZone.c_str());
    didWakeEvent = false;
  }

  // —— NTP：连上网后把真实时间喂给事件日志（每次开机只写一次 NVS）——
  static uint32_t lastNtpTry = 0;
  static bool     ntpSynced  = false;
  if (wifiUp && !ntpSynced && millis() - lastNtpTry > 5000UL) {
    lastNtpTry = millis();
    time_t n = time(nullptr);
    if (n > 1600000000) {          // SNTP 已对上（2020 年后）
      elog.syncTime((uint32_t)n);
      ntpSynced = true;
    }
  }

  // —— PIR 触发 ——
  int zone = pir.pop();
  if (zone >= 0) {
    uint32_t now = millis();
    const char *zn = PirZones::name((uint8_t)zone);
    if (!pir.warmupDone()) {
      Serial.printf("[PIR] 预热中，忽略 %s 的触发\n", zn);
      static uint32_t lastWarmLog = 0;          // 预热期 10 秒最多记一条，防刷爆 CSV
      if (now - lastWarmLog > 10000UL) {
        lastWarmLog = now;
        elog.log(zn, "----", "warmup-ignore");
      }
    } else if (now - lastTrig < policy.cooldownMs) {
      Serial.printf("[PIR] %s 触发，但还在冷却 (还差 %lums)\n",
                    zn, (unsigned long)(policy.cooldownMs - (now - lastTrig)));
      static uint32_t lastCdLog = 0;            // 冷却期 10 秒最多记一条
      if (now - lastCdLog > 10000UL) {
        lastCdLog = now;
        elog.log(zn, "----", "cooldown-skip");
      }
    } else {
      lastTrig = now;
      doWake("PIR", zone);
    }
  }

  // —— 推动播放 ——
  if (playingNow) {
    if (!player.loop()) {
      uint32_t rate = player.rateHz() ? player.rateHz() : 44100;
      uint32_t ms = (uint32_t)((uint64_t)player.samplesPlayed() * 1000 / rate);
      Serial.printf("[音频] 播放结束: %s  %lu 样本 ≈ %lums, 状态:%s\n",
                    playingWhat.c_str(), (unsigned long)player.samplesPlayed(),
                    (unsigned long)ms, player.statusText());
      stopPlayback();
      activeUntil = millis() + 500;
      ampEnable(false);                   // 放完立刻关功放，省几 mA
    }
  }

  // —— 保持时间到 -> 关灯 ——
  if (waking && !playingNow && (int32_t)(millis() - activeUntil) >= 0) {
    waking = false;
    digitalWrite(PIN_LED, LOW);
    if (rgb.ready()) rgb.off();
    Serial.println("[待机] 保持时间结束，等待下一次触发");
  }

  // —— 该睡了 ——
  if (canSleep()) goToSleep();            // 不返回

  delay(2);
}