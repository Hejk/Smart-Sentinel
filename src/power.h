#pragma once
#include <Arduino.h>
#include "config.h"

enum WakeCause : uint8_t {
  WAKE_COLD = 0,    // 上电/复位
  WAKE_PIR,         // PIR 触发唤醒
  WAKE_TIMER,       // 定时醒来同步策略
  WAKE_COOLDOWN,    // 冷却期结束（该回去正常睡了）
  WAKE_OTHER,
};

// ============================================================
//  低功耗管理
//
//  唤醒源：
//    · ext1（3 路 PIR 的 OUT 脚，任一拉高就醒）→ 用来抓动物
//    · 定时器                                   → 醒来同步云端策略
//
//  三个 PIR 脚 GPIO4 / 15 / 16 都在 ESP32-S3 的 RTC IO 范围内
//  （RTC GPIO 只到 GPIO21），所以可以被深睡唤醒。
//
//  ★ PIR 是接在 5V 常电上的，深度睡眠期间它一直在工作，
//    所以唤醒后不用等预热，可以立刻出声。
// ============================================================
class Power {
public:
  void begin();                              // 解析本次唤醒原因

  WakeCause cause() const { return _cause; }
  const char *causeText() const;
  uint32_t pirWakeMask() const;              // 本次唤醒里，哪些 PIR 脚拉高了
  static uint32_t pirMaskAll();              // 三路 PIR 的位掩码

  bool sleepEnabled() const { return _sleepEnabled; }
  void setSleepEnabled(bool on) { _sleepEnabled = on; }

  // 进入深度睡眠（这两个函数都不返回）
  // seconds = 0 表示只靠 PIR 唤醒
  void enterDeepSleep(uint64_t seconds);

  // 冷却睡眠：只开定时器、【关掉 PIR 唤醒】
  //
  // 为什么需要它：深睡后没有时钟了，“上次触发到现在的间隔”算不出来。
  // 所以干脆用“睡多久”当冷却 —— 触发完只睡冷却时长（不开 PIR 唤醒），
  // 到点醒来再回去正常睡。这等价于硬件级的冷却窗口，
  // 而且能防住“动物赖着不走 -> PIR 每 2.5 秒唤醒一次 -> 电池被吃干”。
  void enterCooldownSleep(uint64_t seconds);

  void printWakeInfo() const;

private:
  WakeCause _cause = WAKE_COLD;
  uint32_t  _pirMask = 0;
  bool      _sleepEnabled = SLEEP_ENABLE;
};