#pragma once
#include <Arduino.h>
#include "audio_out.h"

// ============================================================
//  通用 WAV(PCM) 播放器
//  数据源可以是任何 Stream：
//    · 本地文件  File       (LittleFS)
//    · 网络流    WiFiClient / WiFiClientSecure
//  本地和云端共用同一套解析/推流代码，不写两遍。
//
//  支持 8/16bit、单双声道、8k~96k 采样率；双声道只取左声道
//  不支持 MP3 —— 那需要 IDF5 的新 I2S 驱动或换 platform，见第 3 阶段说明
// ============================================================
class WavPlay {
public:
  explicit WavPlay(AudioOut &out) : _out(out) {}

  // netClient 非空时用于判断"网络是否还活着"，本地文件传 nullptr
  bool begin(Stream *src, float gain, Client *netClient = nullptr);
  bool loop();                                  // 主循环里推动；false = 结束或出错
  void stop();

  bool playing() const { return _playing; }
  bool failed()  const { return _err.length() > 0; }
  const char *statusText() const { return _err.length() ? _err.c_str() : "ok"; }

  uint32_t samplesPlayed() const { return _samples; }
  uint32_t bytesRead()     const { return _rx; }
  uint32_t rateHz()        const { return _rate; }

private:
  bool parseHeader();
  bool feed();
  bool alive();
  bool readExact(uint8_t *dst, size_t n, uint32_t timeoutMs);
  bool skipBytes(uint32_t n);
  bool failr(const char *why);
  void fail(const String &why);

  AudioOut &_out;
  Stream   *_src = nullptr;
  Client   *_net = nullptr;

  bool     _playing  = false;
  String   _err;
  uint16_t _channels = 1;
  uint16_t _bits     = 16;
  uint32_t _rate     = 44100;
  uint32_t _dataLeft = 0;
  bool     _infinite = false;
  uint32_t _samples  = 0;
  uint32_t _rx       = 0;
  float    _gain     = 1.0f;

  static const size_t RAW = 1024;
  uint8_t _raw[RAW];
  size_t  _rawLen = 0;
  size_t  _rawPos = 0;

  static const uint32_t READ_TIMEOUT_MS = 8000;
};