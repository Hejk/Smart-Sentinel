#pragma once
#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>

// ============================================================
//  事件日志 —— 每次触发往 SD 卡追加一行 CSV
//
//  文件：/events.csv（卡根目录）
//  列  ：date,time,zone,file,result,src,uptime_ms
//    date/time : 当地时间（NTP 同步后精确；没同步时记 ----）
//    zone      : 1号 / 2号 / 3号 / 串口(串口模拟) / boot
//    file      : 实际播放的 wav 名；被拦截或静音记 ----
//    result    : start / cloud-start / tone-fallback / muted /
//                cooldown-skip / warmup-ignore / boot
//    src       : ntp=真实时间  est=由 NVS 纪元推算  none=没有时间来源
//    uptime_ms : 本次开机后的毫秒数（时间不可信时的排序依据）
//
//  时间戳三级来源：
//    1) NTP（联网后自动同步，精确）
//    2) NVS 里存的纪元 + 运行时长（深睡重启后、NTP 尚未对上时的估算）
//    3) 都没有 -> ----，靠 uptime_ms 排先后
//
//  SD 没挂上时，日志先进 RAM 环形缓冲（16 条），卡挂载成功自动补写。
//  所有写入都避开"播放中段"（在触发时/播放开始前写），防止 SD 写入
//  卡顿造成声音爆音。
// ============================================================
class EventLog {
public:
  // fs 传 nullptr = SD 还没挂上（先只缓冲）。path 形如 /events.csv
  void begin(fs::FS *fs, const char *path);
  // SD 后来才挂上（bank.retrySD() 成功后调用）-> 补挂 + 清空缓冲
  void setFS(fs::FS *fs);

  // detail 非空时作为第 8 列追加（AI 行用：置信度/温湿度/电池），内容里不能带逗号
  void log(const char *zone, const char *file, const char *result,
           const char *detail = "");

  // NTP 同步成功后把当前纪元喂进来（内部去重：同一秒只存一次）
  // 会写入 NVS，深睡/断电重启后还能推出大致时间
  bool syncTime(uint32_t epochNow);
  bool timeValid() const { return _epochBase != 0; }

  void dumpAll();                       // 串口命令 e：打印整个日志
  int  buffered() const { return _nBuf; }

  // —— 后台串口协议（serial_cmd）用的只读访问器 ——
  bool     hasFS() const        { return _fs != nullptr; }
  fs::FS  *filesystem()         { return _fs; }
  const String &path() const    { return _path; }
  String   bufferedLine(int i) const { return (i >= 0 && i < _nBuf) ? _buf[i] : String(); }

private:
  void   writeLine(const String &line);
  uint32_t nowEpoch() const { return _epochBase + millis() / 1000; }

  fs::FS *_fs = nullptr;
  String  _path = "/events.csv";

  static const uint8_t MAX_BUF = 16;
  String  _buf[MAX_BUF];                // SD 未挂载时的环形缓冲（丢弃最旧）
  uint8_t _nBuf = 0;

  Preferences _prefs;
  uint32_t _epochBase = 0;              // millis()=0 对应的真实纪元(秒)，0=未知
  uint32_t _lastNtp   = 0;              // 最近一次 NTP 确认的纪元（去重用）
};
