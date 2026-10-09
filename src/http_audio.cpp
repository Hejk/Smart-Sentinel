#include "http_audio.h"

bool HttpAudio::failr(const char *why) {
  _err = why;
  Serial.printf("[网络音频] 失败: %s\n", why);
  return false;
}

void HttpAudio::close() {
  if (_client) _client->stop();
  _plain.stop();
  _tls.stop();
  _client = nullptr;
}

bool HttpAudio::readLine(String &out, uint32_t timeoutMs) {
  out = "";
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (_client->available() > 0) {
      char c = (char)_client->read();
      if (c == '\n') {
        out.trim();
        return true;
      }
      if (out.length() < 256) out += c;
      t0 = millis();
    }
    if (!_client->connected() && _client->available() <= 0) return out.length() > 0;
    delay(1);
  }
  return false;
}

Client *HttpAudio::open(const String &url) {
  close();
  _err = "";
  _contentType = "";
  _contentLen = -1;
  return doGet(url, 0) ? _client : nullptr;
}

bool HttpAudio::doGet(const String &url, int depth) {
  String u = url;
  u.trim();

  bool tls = false;
  if (u.startsWith("http://")) {
    u = u.substring(7);
  } else if (u.startsWith("https://")) {
    tls = true;
    u = u.substring(8);
  } else {
    return failr("URL 必须以 http:// 或 https:// 开头");
  }

  int slash = u.indexOf('/');
  String hostport = (slash < 0) ? u : u.substring(0, slash);
  String path = (slash < 0) ? "/" : u.substring(slash);
  uint16_t port = tls ? 443 : 80;
  int colon = hostport.indexOf(':');
  if (colon > 0) {
    port = (uint16_t)hostport.substring(colon + 1).toInt();
    hostport = hostport.substring(0, colon);
  }
  if (!hostport.length()) return failr("URL 里没有主机名");

  Serial.printf("[网络音频] 连接 %s:%u%s  (最多等 30 秒)\n", hostport.c_str(), port, path.c_str());
  uint32_t t0 = millis();
  int ok;
  if (tls) {
    _tls.setInsecure();
    _client = &_tls;
    ok = _tls.connect(hostport.c_str(), port);
  } else {
    _client = &_plain;
    ok = _plain.connect(hostport.c_str(), port);
  }
  if (!ok) return failr("连接音频服务器失败（检查 IP/域名、同一局域网、服务是否在跑）");
  Serial.printf("[网络音频] 已连接，耗时 %lums\n", (unsigned long)(millis() - t0));

  String req = String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + hostport + "\r\n"
               "User-Agent: esp32-ir-node\r\n"
               "Connection: close\r\n\r\n";
  _client->print(req);

  String status;
  if (!readLine(status, TIMEOUT_MS)) return failr("没有收到 HTTP 响应");
  int sp = status.indexOf(' ');
  int code = (sp > 0) ? status.substring(sp + 1).toInt() : 0;

  String location, transferEnc;
  for (int i = 0; i < 40; i++) {
    String line;
    if (!readLine(line, TIMEOUT_MS)) break;
    if (!line.length()) break;
    int c = line.indexOf(':');
    if (c < 0) continue;
    String k = line.substring(0, c);
    k.toLowerCase();
    String v = line.substring(c + 1);
    v.trim();
    if      (k == "content-length")    _contentLen = v.toInt();
    else if (k == "content-type")      _contentType = v;
    else if (k == "location")          location = v;
    else if (k == "transfer-encoding") transferEnc = v;
  }

  if (code >= 300 && code < 400 && location.length()) {
    if (depth >= 2) return failr("重定向次数过多");
    close();
    Serial.printf("[网络音频] 跟随重定向 -> %s\n", location.c_str());
    return doGet(location, depth + 1);
  }
  if (code != 200) return failr(String("HTTP 状态码 " + String(code)).c_str());
  if (transferEnc.indexOf("chunked") >= 0) return failr("服务器用了 chunked 传输，暂不支持");
  Serial.printf("[网络音频] HTTP 200  %s  %ld 字节\n", _contentType.c_str(), _contentLen);
  return true;
}