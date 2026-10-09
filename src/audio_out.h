#pragma once
#include <Arduino.h>
#include "config.h"

// MAX98357 (I2S 数字功放) 输出层
//   1) tone()  —— 本地合成提示音，验证 I2S -> 功放 -> 喇叭 链路
//   2) write() —— 直接推 PCM，给云端音频流播放用
// 两条路共用同一个 I2S 通道，采样率可动态切换
class AudioOut {
public:
  bool     begin();                          // 安装 I2S 驱动、绑定引脚
  void     end();                            // 释放 I2S
  bool     ready() const { return _ready; }

  void     setVolume(float v);               // 0.0 ~ 1.0
  float    volume() const { return _vol; }

  uint32_t sampleRate() const { return _rate; }
  bool     setSampleRate(uint32_t hz);       // 播放音频流前按文件头切采样率

  bool     write(const int16_t *samples, size_t count);   // 直接写 PCM
  void     tone(uint16_t freq, uint16_t ms); // 播放单音（带淡入淡出）
  void     chime();                          // 两音提示音
  void     flush();                          // 补齐静音帧，把 DMA 里数据放完

private:
  float    _vol   = 0.6f;
  bool     _ready = false;
  uint32_t _rate  = 44100;
  static const uint32_t SR_DEFAULT = 44100;
};