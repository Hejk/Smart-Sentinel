#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================
//  3 路 HC-SR501 人体红外感应，覆盖三个方向
//
//  特性（和之前的避障模块完全不同，别搞混）：
//    · 检测到活动 -> OUT 拉【高】，并保持约 2.5 秒（模块上可调）
//    · 所以用【上升沿】触发，不是下降沿
//    · 上电后需要 30~60 秒预热，这期间输出可能一直是高，
//      固件会忽略预热期的触发，避免开机就误报
// ============================================================
class PirZones {
public:
  static const uint8_t COUNT = 3;

  // needWarmup: 冷启动要等预热；深度睡眠唤醒时 PIR 一直通着电，不用等
  void begin(bool needWarmup = true);
  int  pop();                              // 取走一个待处理触发 -> 分区号 0~2，-1 = 无
  bool rawActive(uint8_t zone) const;      // 当前该区是否高电平
  bool warmupDone() const;

  static uint8_t     pin(uint8_t zone);
  static const char *name(uint8_t zone);   // "1号" / "2号" / "3号"

  // 供 ISR 调用
  void IRAM_ATTR raise(uint8_t zone) { _flags |= (uint8_t)(1 << zone); }

private:
  volatile uint8_t _flags = 0;             // 每个 bit 对应一个分区
  uint32_t _readyAt = 0;                   // 预热完成时刻
};