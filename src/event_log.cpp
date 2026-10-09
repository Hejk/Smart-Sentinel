#include "event_log.h"

// ---------------------------------------------------------------- 工具
static void fmtEpoch(time_t t, char *db, size_t dn, char *tb, size_t tn) {
  struct tm v;
  localtime_r(&t, &v);
  snprintf(db, dn, "%04d-%02d-%02d", v.tm_year + 1900, v.tm_mon + 1, v.tm_mday);
  snprintf(tb, tn, "%02d:%02d:%02d", v.tm_hour, v.tm_min, v.tm_sec);
}

// ---------------------------------------------------------------- 生命周期
void EventLog::begin(fs::FS *fs, const char *path) {
  _path = path;
  _prefs.begin("evlog", false);                      // 读回上次 NTP 纪元
  _epochBase = _prefs.getUInt("epoch", 0);
  _lastNtp   = _prefs.getUInt("lastntp", 0);
  if (_epochBase) {
    char db[16], tb[12];
    fmtEpoch((time_t)nowEpoch(), db, sizeof(db), tb, sizeof(tb));
    Serial.printf("[日志] 时间基准已从 NVS 恢复（估算=%s %s，NTP 对上后自动校正）\n", db, tb);
  }
  setFS(fs);
}

void EventLog::setFS(fs::FS *fs) {
  if (_fs == fs) return;
  _fs = fs;
  if (!fs) return;
  Serial.printf("[日志] 事件日志就绪: SD %s\n", _path.c_str());
  // 卡挂上后把内存里攒的日志按顺序补写进去
  while (_nBuf) {
    writeLine(_buf[0]);
    for (uint8_t i = 1; i < _nBuf; i++) _buf[i - 1] = _buf[i];
    _nBuf--;
  }
}

// ---------------------------------------------------------------- 记录
void EventLog::log(const char *zone, const char *file, const char *result) {
  char db[16] = "----", tb[12] = "--:--:--";
  const char *src = "none";

  time_t n = time(nullptr);
  if (n > 1600000000) {                              // SNTP 已同步过（2020 年后）
    fmtEpoch(n, db, sizeof(db), tb, sizeof(tb));
    src = "ntp";
  } else if (_epochBase) {                           // 用 NVS 纪元 + 运行时长推算
    fmtEpoch((time_t)nowEpoch(), db, sizeof(db), tb, sizeof(tb));
    src = "est";
  }

  char line[176];
  snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s,%lu",
           db, tb, zone, file, result, src, (unsigned long)millis());

  Serial.printf("[日志] %s %s | %s | %s%s\n", db, tb, zone, file,
                result, _fs ? "" : "  (SD 未挂载，暂存内存)");

  if (_fs) {
    writeLine(String(line));
  } else {
    if (_nBuf >= MAX_BUF) {                          // 满了丢最旧
      for (uint8_t i = 1; i < MAX_BUF; i++) _buf[i - 1] = _buf[i];
      _nBuf--;
    }
    _buf[_nBuf++] = String(line);
  }
}

void EventLog::writeLine(const String &line) {
  File f = _fs->open(_path, FILE_APPEND);
  if (!f) {
    Serial.println("[日志] ✗ 打不开 " + _path + "（写失败，本条丢给内存缓冲）");
    if (_nBuf < MAX_BUF) _buf[_nBuf++] = line;
    return;
  }
  if (f.size() == 0)                                 // 新文件先写表头
    f.println("date,time,zone,file,result,src,uptime_ms");
  f.println(line);
  f.close();
}

// ---------------------------------------------------------------- 时间
bool EventLog::syncTime(uint32_t epochNow) {
  if (epochNow == 0 || epochNow == _lastNtp) return false;   // 没变化不重复写 NVS
  _lastNtp   = epochNow;
  _epochBase = epochNow - millis() / 1000;
  _prefs.putUInt("epoch", _epochBase);
  _prefs.putUInt("lastntp", _lastNtp);
  char db[16], tb[12];
  fmtEpoch((time_t)epochNow, db, sizeof(db), tb, sizeof(tb));
  Serial.printf("[日志] ✓ 时间已同步(NTP): %s %s，并写入 NVS 断电不丢\n", db, tb);
  return true;
}

// ---------------------------------------------------------------- 导出
void EventLog::dumpAll() {
  if (!_fs) {
    Serial.printf("[日志] SD 未挂载，最近 %u 条在内存缓冲：\n", _nBuf);
    for (uint8_t i = 0; i < _nBuf; i++) Serial.println("  │ " + _buf[i]);
    if (!_nBuf) Serial.println("  │ (空)");
    return;
  }
  File f = _fs->open(_path, "r");
  if (!f) {
    Serial.println("[日志] ✗ 打不开 " + _path);
    return;
  }
  Serial.printf("  ┌── 事件日志 %s (%u 字节)%s ──\n", _path.c_str(),
                (unsigned)f.size(), _nBuf ? "  另有内存缓冲未补写" : "");
  int lines = 0;
  while (f.available() && lines < 300) {
    String s = f.readStringUntil('\n');
    s.trim();
    if (s.length()) { Serial.println("  │ " + s); lines++; }
  }
  if (f.available()) Serial.println("  │ ... (超过 300 行截断，文件里还有)");
  else if (!lines)   Serial.println("  │ (空)");
  Serial.println("  └──────────────────────────────────");
  f.close();
}
