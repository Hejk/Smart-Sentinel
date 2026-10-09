#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// ============================================================
//  HTTP 音频取流 —— 只负责把 URL 变成一个"停在响应体开头"的 Stream
//  真正的 WAV 解析和推流交给 WavPlay，两边职责分开
//  支持 http/https(不校验证书) + 最多两跳重定向
// ============================================================
class HttpAudio {
public:
  Client *open(const String &url);        // 失败返回 nullptr，原因看 error()
  void    close();
  bool    opened() const { return _client != nullptr; }

  const char  *error() const { return _err.length() ? _err.c_str() : "ok"; }
  long         contentLength() const { return _contentLen; }
  const String &contentType()  const { return _contentType; }

private:
  bool doGet(const String &url, int depth);
  bool readLine(String &out, uint32_t timeoutMs);
  bool failr(const char *why);

  WiFiClient       _plain;
  WiFiClientSecure _tls;
  Client          *_client = nullptr;
  String           _err;
  String           _contentType;
  long             _contentLen = -1;

  static const uint32_t TIMEOUT_MS = 8000;
};