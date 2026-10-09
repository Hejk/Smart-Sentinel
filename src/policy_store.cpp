#include "policy_store.h"
#include <Preferences.h>

bool PolicyStore::begin() {
  // ★ 一定用读写模式打开：命名空间从没写入过时，只读 begin() 会报
  //   NOT_FOUND，那是「首次上电」而不是「分区坏了」。读写模式两种情况都能开。
  Preferences p;
  if (!p.begin(NS_NAME(), false)) {
    Serial.println("[NVS] ✗ 读写都打不开：分区表里可能没有 NVS 分区，策略无法持久化");
    return false;
  }
  bool   has = p.isKey(KEY());
  size_t len = has ? p.getBytesLength(KEY()) : 0;

  // 开机计数：每次上电/复位 +1。顺带验证「写入→重启→读回」整条 NVS 链路，
  // 深睡调试时它就是唤醒次数计数器。
  _boots = p.getUInt(KEY_BOOTS(), 0) + 1;
  p.putUInt(KEY_BOOTS(), _boots);
  p.end();

  Serial.printf("[NVS] 就绪  开机计数=%u  已保存策略: %s (%u 字节)\n",
                _boots, has ? "有" : "无", (unsigned)len);
  return true;
}

bool PolicyStore::load(String &json) {
  Preferences p;
  if (!p.begin(NS_NAME(), true)) return false;
  if (!p.isKey(KEY())) {
    p.end();
    return false;
  }
  _lastSize = p.getBytesLength(KEY());
  json = p.getString(KEY(), "");
  p.end();

  if (json.length() == 0) return false;
  Serial.printf("[NVS] 读回上次策略 (%u 字节)\n", (unsigned)json.length());
  return true;
}

bool PolicyStore::save(const String &json) {
  Preferences p;
  if (!p.begin(NS_NAME(), false)) {
    Serial.println("[NVS] ✗ 打开失败，策略没存住");
    return false;
  }
  size_t n = p.putString(KEY(), json);
  p.end();
  _lastSize = json.length();
  Serial.printf("[NVS] 策略已存入 NVS (%u 字节)%s\n",
                (unsigned)json.length(), n ? "" : "  ← 可能超出空间");
  return n > 0;
}

void PolicyStore::clear() {
  Preferences p;
  if (!p.begin(NS_NAME(), false)) return;
  p.remove(KEY());
  p.end();
  Serial.println("[NVS] 已清除保存的策略");
}