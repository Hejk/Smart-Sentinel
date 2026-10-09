#pragma once
#include <Arduino.h>
#include "config.h"

class Adafruit_NeoPixel;    // 前向声明，头文件不用拖进整个库

// ============================================================
//  WS2812 RGB 8 位 —— 视觉驱赶
//  鸟对强闪光很敏感，声音 + 闪光双管齐下比单靠声音有效，
//  而且能延缓"脱敏"（动物只习惯声音，不一定习惯突然的强光）
//
//  功耗提醒：8 颗全白最亮约 480mA，会瞬间把电池电压拉低。
//  所以默认亮度压到 RGB_BRIGHTNESS，且只在触发时短促爆闪。
// ============================================================
class LedStrip {
public:
  bool begin();
  void setBrightness(uint8_t b);
  void off();

  void fill(uint8_t r, uint8_t g, uint8_t b);
  void blip(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);   // 闪一下（阻塞，很短）
  void strobe(uint8_t r, uint8_t g, uint8_t b,
              uint8_t times, uint16_t onMs, uint16_t offMs); // 爆闪
  void bootChase();                                          // 开机跑马

  bool ready() const { return _px != nullptr; }

private:
  Adafruit_NeoPixel *_px = nullptr;
  uint8_t _bright = STRIP_BRIGHTNESS;
};