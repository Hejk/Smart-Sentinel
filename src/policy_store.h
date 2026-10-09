#pragma once
#include <Arduino.h>

// ============================================================
//  策略持久化（NVS / Preferences）
//
//  为什么要这个：深度睡眠会把 RAM 清空，每次醒来都是重新开机。
//  云端下发的策略（分区曲库、音量、冷却时间、爆闪参数）必须存下来，
//  否则一睡觉就全忘了、退回固件默认值。
//
//  存的是策略的原始 JSON 字符串，醒来直接 parse 一遍就行，
//  不用为每个字段单独建键值，以后策略加字段也不用改这里。
// ============================================================
class PolicyStore {
public:
  bool begin();
  bool load(String &json);          // 读回上次保存的策略
  bool save(const String &json);    // 收到新策略时调用
  void clear();
  size_t lastSize() const { return _lastSize; }
  uint32_t boots()  const { return _boots; }      // 开机/深睡唤醒计数

private:
  static const char *NS_NAME()   { return "ir-node"; }
  static const char *KEY()       { return "policy"; }
  static const char *KEY_BOOTS() { return "boots"; }
  size_t   _lastSize = 0;
  uint32_t _boots    = 0;
};