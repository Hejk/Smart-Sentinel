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

  // —— V2.1 接线诊断（ESP32 自己当万用表）——
  void lineDiag(bool withAdc);             // 三态判定；withAdc=true 时 ADC 测真实电压
                                           //   （必须在 WiFi 启动前：GPIO15/16 是 ADC2，射频一开就废）
  void beginMonitor(uint32_t ms);          // 实时边沿监听：CHANGE 中断，每根线每次电平变化都打印
  void monitorTick();                      // loop 里调用：打印新边沿；到期自动恢复上升沿触发
  bool monitoring() const { return _monUntil != 0; }

  static uint8_t     pin(uint8_t zone);
  static const char *name(uint8_t zone);   // "1号" / "2号" / "3号"

  // 供 ISR 调用
  void IRAM_ATTR raise(uint8_t zone) {
    _flags |= (uint8_t)(1 << zone);
    _rises[zone < COUNT ? zone : 0]++;        // 接线诊断：上升沿总数（含预热/冷却期）
  }

  // 供监听 ISR 调用（上升沿照常走触发，下降沿只记录）
  void IRAM_ATTR edgeEvent(uint8_t zone, uint8_t level);

  // 接线诊断：该分区自开机以来的上升沿计数
  uint32_t rises(uint8_t zone) const { return (zone < COUNT) ? _rises[zone] : 0; }

private:
  volatile uint8_t  _flags = 0;             // 每个 bit 对应一个分区
  volatile uint32_t _rises[COUNT] = {0};    // 上升沿总数（不受预热/冷却限制）
  uint32_t _readyAt = 0;                   // 预热完成时刻

  // —— 监听模式：ISR 生产 / loop 消费的边沿环形缓冲 ——
  struct EdgeRec { uint32_t t; uint8_t zone; uint8_t lvl; };
  EdgeRec            _eb[48];
  volatile uint8_t   _ew = 0, _er = 0;
  volatile uint16_t  _dropped = 0;
  uint32_t           _monUntil = 0;
};
