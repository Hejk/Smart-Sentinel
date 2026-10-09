#include "serial_cmd.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SD.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "config.h"
#include "sound_bank.h"
#include "event_log.h"
#include "policy.h"
#include "policy_store.h"
#include "audio_out.h"
#include "power.h"

// ----------------------------------------------------------------
// 与 main.cpp 共享的全局对象（单固件，直接 extern，省一套依赖注入）
extern SoundBank    bank;
extern AudioPolicy  policy;
extern PolicyStore  store;
extern EventLog     elog;
extern AudioOut     audio;
extern Power        power;
extern PubSubClient mqtt;
// ----------------------------------------------------------------

// 标准 zlib CRC32（多项式 0xEDB88320），无表实现，慢一点但省 1KB 内存。
// 调用方式：crc 从 0 开始，逐块 crc32Update()，结果即后台 zlib.crc32() 的值。
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// 文件名白名单：只留 [0-9A-Za-z._-]，剥掉一切路径成分（防 ../ 穿越和乱码写坏 FAT）
static String sanitizeName(String n) {
  n.trim();
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  if (n == "." || n == "..") return "";
  String out;
  for (unsigned i = 0; i < n.length(); i++) {
    char ch = n[i];
    bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '_' || ch == '-';
    if (ok) out += ch;
  }
  if (out.length() > 63) out = out.substring(0, 63);   // FAT 短文件名友好
  return out;
}

static void respondDoc(const JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.println();
  Serial.flush();          // USB CDC：等字节真的发出去再返回，后台立刻就能收到
}

static void respondError(const char *cmd, const char *err) {
  JsonDocument r;
  if (cmd) r["cmd"] = cmd;
  r["status"] = "error";
  r["error"]  = err;
  respondDoc(r);
}

// ---------------------------------------------------------------- 生命周期
void SerialCmd::begin(Hooks hooks) {
  _hooks = hooks;
  Serial.println("[协议] 后台串口协议就绪：一行一条 JSON 指令，如 {\"cmd\":\"get_status\"}");
}

bool SerialCmd::charge(char c) {
  if (!_inJson) {
    if (c != '{') return false;          // 不是 JSON，交回单字符命令
    _inJson   = true;
    _overflow = false;
    _line     = "{";
    if (_hooks.activity) _hooks.activity();
    return true;
  }
  if (c == '\n' || c == '\r') {          // 一条指令结束
    _inJson = false;
    if (_overflow) {
      _overflow = false;
      _line     = "";
      respondError(nullptr, "json_too_long");
      return true;
    }
    if (_line.length() > 1) handleLine(_line);
    _line = "";
    return true;
  }
  if (_line.length() >= MAX_LINE - 1) { _overflow = true; return true; }
  _line += c;
  if (_hooks.activity) _hooks.activity();
  return true;
}

void SerialCmd::handleLine(const String &line) {
  JsonDocument cmd;
  DeserializationError err = deserializeJson(cmd, line);
  if (err) {
    String e = String("bad_json: ") + err.c_str();
    respondError(nullptr, e.c_str());
    return;
  }
  const char *c = cmd["cmd"] | (const char *)nullptr;
  if (!c || !strlen(c)) { respondError(nullptr, "no_cmd"); return; }

  String s = c;
  // 串口打印给人看的回显（后台忽略所有非 '{' 开头的行）
  Serial.printf("[协议] ▶ %s\n", s.c_str());

  if      (s == "sync_logs")    cmdSyncLogs(line);
  else if (s == "get_status")   cmdGetStatus();
  else if (s == "upload_audio") cmdUploadAudio(line);
  else if (s == "delete_audio") cmdDeleteAudio(line);
  else if (s == "list_audio")   cmdListAudio();
  else if (s == "set_config")   cmdSetConfig(line);
  else if (s == "get_config")   cmdGetConfig();
  else respondError(s.c_str(), "unknown_cmd");
}

// ---------------------------------------------------------------- sync_logs
// {"cmd":"sync_logs","since":"2025-06-01 12:00:00","limit":200}
//   since 可省（全量）；格式 = 设备日志里的 date+time 拼接，做字符串比较
//   （"YYYY-MM-DD HH:MM:SS" 的字典序即时间序；只给日期也行，当天全回）
//   limit 默认 200，最大 500 —— 防止一次回传几十 KB 卡住串口
void SerialCmd::cmdSyncLogs(const String &line) {
  JsonDocument cmd;
  deserializeJson(cmd, line);
  String since = cmd["since"] | "";
  long   limit = cmd["limit"] | 200L;
  if (limit < 1)   limit = 1;
  if (limit > 500) limit = 500;

  JsonDocument resp;
  resp["cmd"]    = "sync_logs";
  resp["status"] = "ok";
  JsonArray logs = resp["logs"].to<JsonArray>();

  long  count = 0;
  bool  more  = false;

  // 一行 CSV：date,time,zone,file,result,src,uptime_ms
  auto feedCsvLine = [&](const String &raw) -> bool {
    String s = raw;
    s.trim();
    if (!s.length() || s.startsWith("date,")) return true;   // 空行/表头
    String f[7];
    int fi = 0, start = 0;
    while (fi < 6) {
      int comma = s.indexOf(',', start);
      if (comma < 0) break;
      f[fi++] = s.substring(start, comma);
      start = comma + 1;
    }
    f[fi++] = s.substring(start);
    if (fi < 7) return true;                                 // 脏行跳过
    if (since.length()) {
      String dt = f[0] + " " + f[1];
      if (dt < since) return true;                           // 早于 since，不要
    }
    if (count >= limit) { more = true; return false; }
    JsonObject o = logs.add<JsonObject>();
    o["date"]      = f[0];
    o["time"]      = f[1];
    o["zone"]      = f[2];
    o["file"]      = f[3];
    o["result"]    = f[4];
    o["src"]       = f[5];
    o["uptime_ms"] = (uint32_t)strtoul(f[6].c_str(), nullptr, 10);
    count++;
    return true;
  };

  const char *src = nullptr;
  if (elog.filesystem()) {                       // SD 挂着：直读 /events.csv
    File f = elog.filesystem()->open(elog.path(), "r");
    if (!f) { respondError("sync_logs", "log_file_unreadable"); return; }
    while (f.available()) {
      String line2 = f.readStringUntil('\n');
      if (!feedCsvLine(line2)) break;
    }
    f.close();
    src = "sd";
  } else {                                       // SD 没挂：内存环形缓冲（最多 16 条）
    int n = elog.buffered();
    for (int i = 0; i < n; i++)
      if (!feedCsvLine(elog.bufferedLine(i))) break;
    src = "ram_buffer";
  }

  resp["count"]   = count;
  resp["more"]    = more;
  resp["source"]  = src;
  if (more) resp["hint"] = "还有更早的日志，请带上最后一条的时间继续翻页";
  respondDoc(resp);
}

// ---------------------------------------------------------------- get_status
void SerialCmd::cmdGetStatus() {
  JsonDocument resp;
  resp["cmd"]    = "get_status";
  resp["status"] = "ok";

  // 电池：硬件还没接分压检测（18650 走升压直接供电），接上后在这里补 ADC 读数
  resp["battery_voltage"] = -1;
  resp["battery"]         = "未接入(硬件无分压检测)";

  float t = 0, h = 0;
  if (_hooks.readClimate && _hooks.readClimate(t, h)) {
    resp["temperature"] = (int)(t * 10 + 0.5f) / 10.0f;   // 一位小数，减串口垃圾
    resp["humidity"]    = (int)(h + 0.5f);
  } else {
    resp["temperature"] = nullptr;
    resp["humidity"]    = nullptr;
  }

  // 容量：fs::FS 基类没有 totalBytes，按当前后端分别取
  if (bank.filesystem()) {
    uint64_t total = 0, used = 0;
    if (bank.sdMounted()) {
      total = SD.totalBytes();
      used  = SD.usedBytes();
    } else {
      total = LittleFS.totalBytes();
      used  = LittleFS.usedBytes();
    }
    resp["sd_total_mb"] = (uint32_t)(total / (1024ULL * 1024ULL));
    resp["sd_used_mb"]  = (uint32_t)(used  / (1024ULL * 1024ULL));
  } else {
    resp["sd_total_mb"] = nullptr;
    resp["sd_used_mb"]  = nullptr;
  }
  resp["storage"]      = bank.backendName();
  resp["sd_mounted"]   = bank.sdMounted();
  resp["audio_count"]  = bank.count();
  resp["uptime_seconds"] = millis() / 1000;
  resp["free_heap_kb"]   = ESP.getFreeHeap() / 1024;
  resp["wake_cause"]     = power.causeText();
  resp["mqtt_connected"] = mqtt.connected();
  if (WiFi.status() == WL_CONNECTED) {
    resp["wifi_rssi"] = WiFi.RSSI();
    resp["ip"]        = WiFi.localIP().toString();
  } else {
    resp["wifi_rssi"] = nullptr;
  }

  time_t now = time(nullptr);
  if (now > 1600000000) {
    char db[20];
    struct tm v;
    localtime_r(&now, &v);
    strftime(db, sizeof(db), "%Y-%m-%d %H:%M:%S", &v);
    resp["time"] = db;
  } else {
    resp["time"] = nullptr;              // NTP 没对上，后台别信设备时间
  }
  respondDoc(resp);
}

// ---------------------------------------------------------------- upload_audio
// {"cmd":"upload_audio","filename":"hawk_scream.wav","size":152644,"crc32":123456789}
//   crc32 可选（后台用 zlib.crc32(data) 算）。设备回 {"status":"ready"} 后，
//   后台【再】开始灌二进制流，灌完设备校验并回最终结果。
void SerialCmd::cmdUploadAudio(const String &line) {
  JsonDocument cmd;
  deserializeJson(cmd, line);

  String filename = sanitizeName(cmd["filename"] | "");
  size_t size     = (size_t)(uint64_t)(cmd["size"] | 0L);
  bool   hasCrc   = !cmd["crc32"].isNull();
  uint32_t crcWant = (uint32_t)(uint64_t)(cmd["crc32"] | 0L);

  if (!filename.length()) { respondError("upload_audio", "bad_filename"); return; }
  if (size == 0 || size > 8 * 1024 * 1024UL) { respondError("upload_audio", "bad_size"); return; }
  fs::FS *fs = bank.filesystem();
  if (!fs) { respondError("upload_audio", "no_storage"); return; }

  // SD 和播放不能并发：先停掉正在放的音频
  if (_hooks.isPlaying && _hooks.isPlaying() && _hooks.stopPlayback)
    _hooks.stopPlayback();

  String path = String(bank.rootPath()) + "/" + filename;

  JsonDocument ready;
  ready["cmd"]    = "upload_audio";
  ready["status"] = "ready";
  ready["path"]   = path;
  respondDoc(ready);                     // 后台收到这条才开始发数据

  // 文件的 open/close 全在 receiveBinary 里（同一文件不能开两个写句柄）
  String err;
  size_t got = 0;
  if (!receiveBinary(path, size, crcWant, hasCrc, err, got)) {
    fs->remove(path);                    // 半截文件不留尸
    JsonDocument r;
    r["cmd"] = "upload_audio"; r["status"] = "error";
    r["error"] = err; r["received"] = got;
    respondDoc(r);
    return;
  }

  bank.refresh();                        // 新音频立刻进曲库，下个触发就能用
  JsonDocument r;
  r["cmd"] = "upload_audio"; r["status"] = "ok";
  r["filename"] = filename; r["size"] = got;
  if (hasCrc) r["crc32"] = crcWant;
  r["audio_count"] = bank.count();
  respondDoc(r);
}

bool SerialCmd::receiveBinary(const String &path, size_t size,
                              uint32_t crcWant, bool hasCrc,
                              String &err, size_t &got) {
  static uint8_t buf[1024];
  File f = bank.filesystem()->open(path, FILE_WRITE);
  if (!f) { err = "open_failed"; return false; }

  uint32_t crcVal   = 0;
  uint32_t lastData = millis();
  uint32_t t0       = millis();

  while (got < size) {
    size_t want = size - got;
    if (want > sizeof(buf)) want = sizeof(buf);
    int n = Serial.read(buf, want);      // HWCDC：收多少给多少，没有给 0
    if (n > 0) {
      if (f.write(buf, n) != (size_t)n) {
        f.close(); err = "sd_write_failed"; return false;
      }
      crcVal = crc32Update(crcVal, buf, n);
      got += n;
      lastData = millis();
      if (_hooks.activity) _hooks.activity();   // 大文件接收期间不许睡
    } else {
      if (millis() - lastData > 15000UL) {       // 15 秒没数据 = 后台掉线/中断
        f.close(); err = "timeout_waiting_data"; return false;
      }
      if (millis() - t0 > 600000UL) {            // 总时长 10 分钟兜底
        f.close(); err = "total_timeout"; return false;
      }
      delay(1);                                  // 让路给系统任务（USB/蓝牙栈）
    }
  }
  f.close();

  if (hasCrc && crcVal != crcWant) {
    err = "crc_mismatch";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- delete_audio
void SerialCmd::cmdDeleteAudio(const String &line) {
  JsonDocument cmd;
  deserializeJson(cmd, line);
  String filename = sanitizeName(cmd["filename"] | "");
  if (!filename.length()) { respondError("delete_audio", "bad_filename"); return; }

  fs::FS *fs = bank.filesystem();
  if (!fs) { respondError("delete_audio", "no_storage"); return; }

  // 正在放要删的这条 -> 先停，否则句柄悬空
  if (_hooks.isPlaying && _hooks.isPlaying() && _hooks.stopPlayback)
    _hooks.stopPlayback();

  String path = String(bank.rootPath()) + "/" + filename;
  if (!fs->exists(path)) { respondError("delete_audio", "not_found"); return; }
  if (!fs->remove(path)) { respondError("delete_audio", "remove_failed"); return; }

  bank.refresh();
  JsonDocument r;
  r["cmd"] = "delete_audio"; r["status"] = "ok";
  r["filename"] = filename; r["audio_count"] = bank.count();
  respondDoc(r);
}

// ---------------------------------------------------------------- list_audio
void SerialCmd::cmdListAudio() {
  JsonDocument resp;
  resp["cmd"]    = "list_audio";
  resp["status"] = "ok";

  fs::FS *fs = bank.filesystem();
  if (!fs) { respondError("list_audio", "no_storage"); return; }

  resp["backend"] = bank.backendName();
  resp["root"]    = bank.rootPath();
  JsonArray files = resp["files"].to<JsonArray>();
  int count = 0;
  File dir = fs->open(bank.rootPath());
  if (!dir || !dir.isDirectory()) { respondError("list_audio", "dir_missing"); return; }

  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) {
      String n = e.name();
      int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (n.endsWith(".wav") || n.endsWith(".WAV")) {
        JsonObject o = files.add<JsonObject>();
        o["name"] = n;
        o["size"] = (uint32_t)e.size();
        count++;
      }
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  resp["count"] = count;
  respondDoc(resp);
}

// ---------------------------------------------------------------- config
// 两种写法都收：
//   {"cmd":"set_config","key":"volume","value":"0.8"}
//   {"cmd":"set_config","config":{"volume":"0.8","trigger_interval":"30"}}
// 改完存 NVS（深度睡眠/断电不丢）。
// ★ 注意：这是"本地兜底配置"，云端 MQTT 下发策略后会覆盖它。
void SerialCmd::cmdSetConfig(const String &line) {
  JsonDocument cmd;
  deserializeJson(cmd, line);

  JsonVariant body = cmd["config"];
  if (body.isNull()) {                   // 单条 key/value 形式，包成对象统一处理
    JsonDocument one;
    JsonObject o = one.to<JsonObject>();
    const char *k = cmd["key"] | (const char *)nullptr;
    if (!k) { respondError("set_config", "no_key"); return; }
    o[k] = cmd["value"];
    body = one.as<JsonObject>();
  }

  JsonDocument resp;
  resp["cmd"]    = "set_config";
  resp["status"] = "ok";
  JsonArray okArr  = resp["applied"].to<JsonArray>();
  JsonArray badArr = resp["unknown_keys"].to<JsonArray>();

  for (JsonPair kv : body.as<JsonObject>()) {
    String k = kv.key().c_str();
    String v;
    if      (kv.value().is<const char *>()) v = kv.value().as<const char *>();
    else if (kv.value().is<bool>())         v = kv.value().as<bool>() ? "1" : "0";
    else if (kv.value().is<long>())         v = String((long)kv.value().as<long long>());
    else if (kv.value().is<double>())       v = String(kv.value().as<double>(), 3);
    else v = kv.value().as<String>();
    v.trim();

    bool ok = true;
    if      (k == "volume") {
      float f = v.toFloat();
      policy.volume = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
      audio.setVolume(policy.volume);
    }
    else if (k == "trigger_interval") {                       // 秒（后台语义）
      long ms = v.toFloat() * 1000.0f;
      policy.cooldownMs = (ms < 200) ? 200 : (uint32_t)ms;
    }
    else if (k == "cooldown_ms") {                            // 毫秒（固件原生）
      long ms = v.toFloat();
      policy.cooldownMs = (ms < 200) ? 200 : (uint32_t)ms;
    }
    else if (k == "hold_ms") {
      long ms = v.toFloat();
      policy.holdMs = (ms < 200) ? 200 : (uint32_t)ms;
    }
    else if (k == "tone_hz")  policy.toneHz = clampU16(v.toInt(), 100, 8000);
    else if (k == "tone_ms")  policy.toneMs = clampU16(v.toInt(), 20, 3000);
    else if (k == "mode")     policy.mode = (v == "mute") ? "mute" : "trigger";
    else if (k == "enabled")  policy.enabled = !(v == "0" || v == "false" || v == "off");
    else if (k == "pick")     policy.pick = (v == "round") ? "round" : "random";
    else if (k == "source")   policy.source = (v == "cloud") ? "cloud" : "local";
    else if (k == "sounds")   policy.sounds = v;
    else if (k == "clip_url") policy.clipUrl = v;
    else if (k == "flash_enabled") policy.flash = !(v == "0" || v == "false" || v == "off");
    else if (k == "flash_count")   policy.flashCount = clampU16(v.toInt(), 1, 20);
    else if (k == "flash_ms")      policy.flashMs    = clampU16(v.toInt(), 20, 1000);
    else if (k == "flash_rgb") {                              // "255,80,0"
      int r = 0, g = 0, b = 0;
      if (sscanf(v.c_str(), "%d , %d , %d", &r, &g, &b) == 3) {
        policy.flashR = (uint8_t)constrain(r, 0, 255);
        policy.flashG = (uint8_t)constrain(g, 0, 255);
        policy.flashB = (uint8_t)constrain(b, 0, 255);
      } else ok = false;
    }
    else ok = false;

    if (ok) okArr.add(k);
    else    badArr.add(k);
  }

  if (okArr.size() > 0) {
    // 存 NVS：深睡/断电后 set_config 的结果还在（云端策略来了会被覆盖，见文档）
    String json = serializePolicyToString(policy);
    store.save(json);
  }
  if (badArr.size() > 0 && okArr.size() == 0) resp["status"] = "error";
  respondDoc(resp);
}

void SerialCmd::cmdGetConfig() {
  JsonDocument resp;
  resp["cmd"]    = "get_config";
  resp["status"] = "ok";
  JsonObject c   = resp["config"].to<JsonObject>();

  char buf[24];
  snprintf(buf, sizeof(buf), "%.2f", policy.volume);
  c["volume"]           = buf;                             // 0.00 ~ 1.00
  c["enabled"]          = policy.enabled ? "1" : "0";
  c["mode"]             = policy.mode;
  c["trigger_interval"] = String(policy.cooldownMs / 1000); // 秒，后台用
  c["cooldown_ms"]      = String((unsigned long)policy.cooldownMs);
  c["hold_ms"]          = String((unsigned long)policy.holdMs);
  snprintf(buf, sizeof(buf), "%u", policy.toneHz);
  c["tone_hz"] = buf;
  snprintf(buf, sizeof(buf), "%u", policy.toneMs);
  c["tone_ms"]  = buf;
  c["pick"]     = policy.pick;
  c["source"]   = policy.source;
  c["sounds"]   = policy.sounds;
  c["clip_url"] = policy.clipUrl;
  c["flash_enabled"] = policy.flash ? "1" : "0";
  snprintf(buf, sizeof(buf), "%u,%u,%u", policy.flashR, policy.flashG, policy.flashB);
  c["flash_rgb"]   = buf;
  snprintf(buf, sizeof(buf), "%u", policy.flashCount);
  c["flash_count"] = buf;
  snprintf(buf, sizeof(buf), "%u", policy.flashMs);
  c["flash_ms"] = buf;

  respondDoc(resp);
}
