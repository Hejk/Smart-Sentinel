#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
//  云端下发的“音频策略”
//  设备订阅 sa/ir-node-<MAC>/policy，收到即生效（retained 消息开机就会收到）
//
//  例：
//  {
//    "name": "demo",
//    "enabled": true,
//    "mode": "trigger",        // trigger=只在红外触发时放音, mute=静音
//    "volume": 0.6,            // 0.0 ~ 1.0
//    "tone_hz": 1200,          // 提示音频率
//    "tone_ms": 250,           // 提示音时长
//    "cooldown_ms": 3000,      // 两次触发的最小间隔
//    "hold_ms": 5000,          // 唤醒后保持点亮的时长
//    "clip_url": ""            // 云端音频地址（下一阶段接入播放）
//  }
//  也兼容最简写法：payload 直接是一段 http 开头的音频地址
// ============================================================
struct AudioPolicy {
  String   name       = "default";
  bool     enabled    = true;
  String   mode       = "trigger";
  float    volume     = 0.6f;
  uint16_t toneHz     = 1200;
  uint16_t toneMs     = 250;
  uint32_t cooldownMs = 3000;
  uint32_t holdMs     = 5000;
  String   clipUrl    = "";
  String   sounds     = "";        // 逗号分隔的本地音频候选；空 = 用 Flash 里全部
  String   pick       = "random";   // random = 随机不重复 | round = 顺序轮换
  String   source     = "local";    // local = 优先本地 Flash | cloud = 优先云端 URL

  // 3 路 PIR 各自的曲库（空 = 用全局 sounds）
  // 例：1 号在果园边 -> 放猛禽声；3 号在路边 -> 放狗吠
  String   zoneSounds[3];

  // 触发时的视觉爆闪（鸟对强光敏感）
  bool     flash      = true;
  uint8_t  flashR     = 255;
  uint8_t  flashG     = 80;
  uint8_t  flashB     = 0;
  uint16_t flashCount = 3;
  uint16_t flashMs    = 90;

  bool     fromCloud  = false;    // 是否已被云端策略覆盖
};

static inline uint16_t clampU16(long v, long lo, long hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (uint16_t)v;
}

static inline bool parsePolicy(const char *json, AudioPolicy &p) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return false;

  if (doc["name"].is<const char *>())       p.name       = doc["name"].as<const char *>();
  if (doc["enabled"].is<bool>())            p.enabled    = doc["enabled"].as<bool>();
  if (doc["mode"].is<const char *>())       p.mode       = doc["mode"].as<const char *>();
  if (doc["volume"].is<float>())            p.volume     = doc["volume"].as<float>();
  if (doc["tone_hz"].is<long>())            p.toneHz     = clampU16(doc["tone_hz"].as<long>(), 100, 8000);
  if (doc["tone_ms"].is<long>())            p.toneMs     = clampU16(doc["tone_ms"].as<long>(), 20, 3000);
  if (doc["cooldown_ms"].is<long>())        p.cooldownMs = (uint32_t)doc["cooldown_ms"].as<long>();
  if (doc["hold_ms"].is<long>())            p.holdMs     = (uint32_t)doc["hold_ms"].as<long>();
  if (doc["clip_url"].is<const char *>())   p.clipUrl    = doc["clip_url"].as<const char *>();

  // sounds 同时接受两种写法：字符串 "a.wav,b.wav" 或 JSON 数组 ["a.wav","b.wav"]
  if (doc["sounds"].is<JsonArray>()) {
    p.sounds = "";
    for (JsonVariant v : doc["sounds"].as<JsonArray>()) {
      if (!v.is<const char *>()) continue;
      if (p.sounds.length()) p.sounds += ",";
      p.sounds += v.as<const char *>();
    }
  } else if (doc["sounds"].is<const char *>()) {
    p.sounds = doc["sounds"].as<const char *>();
  }
  if (doc["pick"].is<const char *>())   p.pick   = doc["pick"].as<const char *>();
  if (doc["source"].is<const char *>()) p.source = doc["source"].as<const char *>();

  // zones: {"1": ["a.wav"], "2": "b.wav"} —— 1~3 对应三路 PIR
  if (doc["zones"].is<JsonObject>()) {
    JsonObject z = doc["zones"].as<JsonObject>();
    for (uint8_t i = 0; i < 3; i++) {
      char key[2] = { (char)('1' + i), 0 };
      JsonVariant v = z[key];
      if (v.is<JsonArray>()) {
        String s;
        for (JsonVariant it : v.as<JsonArray>()) {
          if (!it.is<const char *>()) continue;
          if (s.length()) s += ",";
          s += it.as<const char *>();
        }
        p.zoneSounds[i] = s;
      } else if (v.is<const char *>()) {
        p.zoneSounds[i] = v.as<const char *>();
      }
    }
  }

  // flash: {"enabled":true, "rgb":[255,80,0], "count":3, "ms":90}
  if (doc["flash"].is<JsonObject>()) {
    JsonObject f = doc["flash"].as<JsonObject>();
    if (f["enabled"].is<bool>()) p.flash = f["enabled"].as<bool>();
    if (f["rgb"].is<JsonArray>()) {
      JsonArray a = f["rgb"].as<JsonArray>();
      if (a.size() >= 3) {
        p.flashR = a[0].as<uint8_t>();
        p.flashG = a[1].as<uint8_t>();
        p.flashB = a[2].as<uint8_t>();
      }
    }
    if (f["count"].is<long>()) p.flashCount = clampU16(f["count"].as<long>(), 1, 20);
    if (f["ms"].is<long>())    p.flashMs    = clampU16(f["ms"].as<long>(), 20, 1000);
  }

  if (p.volume    < 0.0f) p.volume = 0.0f;
  if (p.volume    > 1.0f) p.volume = 1.0f;
  if (p.cooldownMs < 200) p.cooldownMs = 200;
  if (p.holdMs    < 200)  p.holdMs = 200;

  p.fromCloud = true;
  return true;
}

// 兼容“payload 就是一个音频地址”的极简下发
static inline bool parsePolicyOrUrl(const char *payload, AudioPolicy &p) {
  while (*payload == ' ') payload++;
  if (strncmp(payload, "http", 4) == 0) {
    p.clipUrl   = payload;
    p.fromCloud = true;
    return true;
  }
  return parsePolicy(payload, p);
}

// 把当前策略序列化回 JSON（parse 的逆操作）。
// 用途：串口 set_config 改完字段后存 NVS —— 深睡醒来 parse 的还是这份 JSON，
// 所以必须保证序列化结果能被 parsePolicy 无损读回。
static inline String serializePolicyToString(const AudioPolicy &p) {
  JsonDocument d;
  d["name"]       = p.name;
  d["enabled"]    = p.enabled;
  d["mode"]       = p.mode;
  d["volume"]     = p.volume;
  d["tone_hz"]    = p.toneHz;
  d["tone_ms"]    = p.toneMs;
  d["cooldown_ms"] = p.cooldownMs;
  d["hold_ms"]    = p.holdMs;
  d["clip_url"]   = p.clipUrl;
  d["pick"]       = p.pick;
  d["source"]     = p.source;

  JsonArray s = d["sounds"].to<JsonArray>();
  if (p.sounds.length()) {
    int start = 0;
    while (start < (int)p.sounds.length()) {
      int comma = p.sounds.indexOf(',', start);
      String item = (comma < 0) ? p.sounds.substring(start)
                                : p.sounds.substring(start, comma);
      item.trim();
      if (item.length()) s.add(item);
      if (comma < 0) break;
      start = comma + 1;
    }
  }

  JsonObject z = d["zones"].to<JsonObject>();
  for (uint8_t i = 0; i < 3; i++) {
    if (!p.zoneSounds[i].length()) continue;
    JsonArray zz = z[String((char)('1' + i))].to<JsonArray>();
    int start = 0;
    while (start < (int)p.zoneSounds[i].length()) {
      int comma = p.zoneSounds[i].indexOf(',', start);
      String item = (comma < 0) ? p.zoneSounds[i].substring(start)
                                : p.zoneSounds[i].substring(start, comma);
      item.trim();
      if (item.length()) zz.add(item);
      if (comma < 0) break;
      start = comma + 1;
    }
  }

  JsonObject f = d["flash"].to<JsonObject>();
  f["enabled"] = p.flash;
  JsonArray rgb = f["rgb"].to<JsonArray>();
  rgb.add(p.flashR); rgb.add(p.flashG); rgb.add(p.flashB);
  f["count"] = p.flashCount;
  f["ms"]    = p.flashMs;

  String out;
  serializeJson(d, out);
  return out;
}

static inline void printPolicy(const AudioPolicy &p) {
  Serial.println("  ┌── 当前生效策略 ──────────────────");
  Serial.printf("  │ 来源      : %s\n", p.fromCloud ? "云端 MQTT" : "固件默认");
  Serial.printf("  │ 名称      : %s\n", p.name.c_str());
  Serial.printf("  │ 状态      : %s  模式=%s\n", p.enabled ? "启用" : "禁用", p.mode.c_str());
  Serial.printf("  │ 音量      : %.2f\n", p.volume);
  Serial.printf("  │ 提示音    : %uHz / %ums\n", p.toneHz, p.toneMs);
  Serial.printf("  │ 冷却/保持 : %lums / %lums\n",
                (unsigned long)p.cooldownMs, (unsigned long)p.holdMs);
  Serial.printf("  │ 云端音频  : %s\n", p.clipUrl.length() ? p.clipUrl.c_str() : "(未下发)");
  Serial.printf("  │ 本地音频  : %s  (挑选=%s)\n",
                p.sounds.length() ? p.sounds.c_str() : "全部", p.pick.c_str());
  Serial.printf("  │ 优先来源  : %s\n", p.source.c_str());
  Serial.printf("  │ 分区曲库  : 1号=%s 2号=%s 3号=%s\n",
                p.zoneSounds[0].length() ? p.zoneSounds[0].c_str() : "(全局)",
                p.zoneSounds[1].length() ? p.zoneSounds[1].c_str() : "(全局)",
                p.zoneSounds[2].length() ? p.zoneSounds[2].c_str() : "(全局)");
  Serial.printf("  │ 视觉爆闪  : %s  RGB(%u,%u,%u) ×%u 每下%ums\n",
                p.flash ? "开" : "关", p.flashR, p.flashG, p.flashB, p.flashCount, p.flashMs);
  Serial.println("  └──────────────────────────────────");
}