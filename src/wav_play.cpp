#include "wav_play.h"

#include <string.h>

// ---------------------------------------------------------------- 内部工具
bool WavPlay::failr(const char *why) {
  fail(why);
  return false;
}

void WavPlay::fail(const String &why) {
  _err = why;
  Serial.printf("[音频] 失败: %s\n", why.c_str());
}

bool WavPlay::alive() {
  if (!_src) return false;
  if (_net) return _net->connected() || _net->available() > 0;
  return true;                       // 本地文件：靠读不到数据来判断结束
}

bool WavPlay::readExact(uint8_t *dst, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    int avail = _src->available();
    if (avail > 0) {
      size_t want = ((size_t)avail < (n - got)) ? (size_t)avail : (n - got);
      int r = _src->readBytes((char *)(dst + got), want);
      if (r > 0) {
        got += r;
        _rx += r;
        t0 = millis();
        continue;
      }
    }
    if (!alive()) return false;
    if (millis() - t0 > timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool WavPlay::skipBytes(uint32_t n) {
  uint8_t tmp[64];
  while (n > 0) {
    size_t chunk = (n > sizeof(tmp)) ? sizeof(tmp) : n;
    if (!readExact(tmp, chunk, READ_TIMEOUT_MS)) return false;
    n -= chunk;
  }
  return true;
}

// ---------------------------------------------------------------- WAV 头
bool WavPlay::parseHeader() {
  uint8_t hdr[12];
  if (!readExact(hdr, 12, READ_TIMEOUT_MS)) return failr("读取 WAV 头失败");
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    return failr("这不是 WAV 文件（RIFF/WAVE 标记不对）");

  for (int guard = 0; guard < 32; guard++) {
    uint8_t ch[8];
    if (!readExact(ch, 8, READ_TIMEOUT_MS)) return failr("WAV 分块信息读取失败");
    uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                    ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

    if (memcmp(ch, "fmt ", 4) == 0) {
      if (size < 16) return failr("fmt 块过小");
      uint8_t f[16];
      if (!readExact(f, 16, READ_TIMEOUT_MS)) return failr("读取 fmt 块失败");
      uint16_t fmt = (uint16_t)(f[0] | (f[1] << 8));
      _channels = (uint16_t)(f[2] | (f[3] << 8));
      _rate = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
              ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
      _bits = (uint16_t)(f[14] | (f[15] << 8));
      if (size > 16 && !skipBytes(size - 16)) return failr("跳过 fmt 扩展失败");
      if ((size & 1) && !skipBytes(1)) return failr("跳过 fmt 填充失败");

      if (fmt != 1)                        return failr("只支持 PCM 编码的 WAV");
      if (_channels < 1 || _channels > 2)  return failr("声道数不支持（只支持单/双声道）");
      if (_bits != 16 && _bits != 8)       return failr("只支持 8bit / 16bit 的 WAV");
    } else if (memcmp(ch, "data", 4) == 0) {
      if (_rate < 8000 || _rate > 96000) return failr("采样率超出支持范围");
      _dataLeft = size;
      _infinite = (size == 0 || size >= 0x7FFFFFFF);
      Serial.printf("[音频] WAV %luHz %u声道 %ubit, 数据 %s\n",
                    (unsigned long)_rate, _channels, _bits,
                    _infinite ? "长度未知(流式)" : String(size).c_str());
      if (!_out.setSampleRate(_rate)) return failr("I2S 采样率切换失败");
      return true;
    } else {
      if (!skipBytes(size + (size & 1))) return failr("跳过未知分块失败");
    }
  }
  return failr("WAV 头里没找到 data 块");
}

// ---------------------------------------------------------------- 取一块 -> I2S
bool WavPlay::feed() {
  const size_t frameBytes = (size_t)(_bits / 8) * _channels;

  if (_rawPos + frameBytes > _rawLen) {
    size_t rest = _rawLen - _rawPos;
    if (rest) memmove(_raw, _raw + _rawPos, rest);
    _rawPos = 0;
    _rawLen = rest;

    uint32_t t0 = millis();
    while (_rawLen < RAW) {
      int avail = _src->available();
      if (avail <= 0) {
        if (_rawLen >= frameBytes && millis() - t0 > 3) break;   // 手里有货先处理
        if (!alive()) break;                                     // 源结束了
        if (millis() - t0 > READ_TIMEOUT_MS) return failr("等待音频数据超时");
        delay(1);
        continue;
      }
      size_t want = ((size_t)avail < (RAW - _rawLen)) ? (size_t)avail : (RAW - _rawLen);
      int r = _src->readBytes((char *)(_raw + _rawLen), want);
      if (r <= 0) break;
      _rawLen += r;
      _rx += r;
    }
    if (_rawLen < frameBytes) return false;
  }

  static int16_t out[512];
  size_t samples = 0;
  size_t frames = 0;
  while (_rawPos + frameBytes <= _rawLen && samples < 512) {
    int32_t s;
    if (_bits == 16) {
      s = (int16_t)((uint16_t)_raw[_rawPos] | ((uint16_t)_raw[_rawPos + 1] << 8));
    } else {
      s = ((int32_t)_raw[_rawPos] - 128) << 8;      // 8bit 无符号
    }
    _rawPos += frameBytes;
    frames++;
    int32_t v = (int32_t)(s * _gain);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out[samples++] = (int16_t)v;
  }

  if (samples) {
    _out.write(out, samples);
    _samples += samples;
    if (!_infinite) {
      uint32_t used = (uint32_t)frames * (uint32_t)frameBytes;
      _dataLeft = (used > _dataLeft) ? 0 : (_dataLeft - used);
    }
  }
  return true;
}

// ---------------------------------------------------------------- 对外接口
bool WavPlay::begin(Stream *src, float gain, Client *netClient) {
  stop();
  _src = src;
  _net = netClient;
  _err = "";
  _samples = 0;
  _rx = 0;
  _rawLen = _rawPos = 0;
  _infinite = false;
  _dataLeft = 0;
  _gain = gain;

  if (!_src) return failr("数据源为空");
  if (!parseHeader()) return false;

  _playing = true;
  return true;
}

bool WavPlay::loop() {
  if (!_playing) return false;

  if (!_infinite && _dataLeft == 0) {
    _out.flush();
    stop();
    return false;
  }
  if (!feed()) {
    if (!failed()) _out.flush();
    stop();
    return false;
  }
  if (_infinite && !_net) {           // 流式 WAV 但数据源已结束
    _out.flush();
    stop();
    return false;
  }
  if (_infinite && _net && !_net->connected() && _net->available() <= 0) {
    _out.flush();
    stop();
    return false;
  }
  return true;
}

void WavPlay::stop() {
  _playing = false;
  _src = nullptr;
  _net = nullptr;
}