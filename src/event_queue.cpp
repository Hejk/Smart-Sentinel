#include "event_queue.h"
#include <WiFi.h>

namespace evq {

// ------------------------------------------------------------ 队列本体
static constexpr uint8_t Q_CAP = 24;         // 环形队列容量

struct Item {
  uint32_t seq;                              // 全局自增序号，补报保持顺序
  uint32_t upMs;                             // 入队时的 millis()
  char     event[20];
  char     detail[48];
};

static Item   s_q[Q_CAP];
static uint8_t s_head = 0;                   // 下一个出队位置
static uint8_t s_cnt  = 0;
static uint32_t s_seq = 0;
static uint32_t s_dropped = 0;

void begin() {
  s_head = 0; s_cnt = 0; s_seq = 0; s_dropped = 0;
}

uint8_t pending() { return s_cnt; }
uint32_t dropped() { return s_dropped; }

// ------------------------------------------------------------ 入队
static bool pushLocked(const char *event, const char *detail) {
  if (s_cnt >= Q_CAP) {                      // 满了：挤掉最旧的一条
    s_head = (s_head + 1) % Q_CAP;
    s_cnt--;
    s_dropped++;
    Serial.println("[队列] ⚠ 已满，挤掉最旧的一条事件");
  }
  Item &it = s_q[(s_head + s_cnt) % Q_CAP];
  it.seq = ++s_seq;
  it.upMs = millis();
  strncpy(it.event,  event,  sizeof(it.event)  - 1); it.event[sizeof(it.event) - 1]   = 0;
  strncpy(it.detail, detail, sizeof(it.detail) - 1); it.detail[sizeof(it.detail) - 1] = 0;
  s_cnt++;
  return true;
}

// ------------------------------------------------------------ 发布
static bool publishJson(PubSubClient &mqtt, const String &topic,
                        const char *event, const char *detail,
                        const String &policyName, bool offline,
                        uint32_t upMs, uint32_t seq) {
  char buf[224];
  int n = snprintf(buf, sizeof(buf),
      "{\"event\":\"%s\",\"detail\":\"%s\",\"policy\":\"%s\",\"uptime_ms\":%lu,\"rssi\":%d,\"offline\":%s",
      event, detail, policyName.c_str(), (unsigned long)upMs, WiFi.RSSI(),
      offline ? "true" : "false");
  if (offline) {                             // 补报事件多带两个信息：排队多久了 + 自己的序号
    snprintf(buf + n, sizeof(buf) - n, ",\"age_ms\":%lu,\"seq\":%lu}",
             (unsigned long)(millis() - upMs), (unsigned long)seq);
  } else {
    snprintf(buf + n, sizeof(buf) - n, "}");
  }
  return mqtt.publish(topic.c_str(), buf);
}

// ------------------------------------------------------------ 统一入口
bool submit(PubSubClient &mqtt, const String &topic, const String &policyName,
            const char *event, const char *detail) {
  if (mqtt.connected()) {
    bool ok = publishJson(mqtt, topic, event, detail, policyName, false, millis(), 0);
    if (ok) {
      Serial.printf("[上报] %s (%s)\n", event, detail);
      return true;
    }
    Serial.println("[上报] ✗ 发布失败（代理没收到），转入离线队列");
  }
  pushLocked(event, detail);
  Serial.printf("[队列] 事件入队待补报: %s (%s)  排队 %u 条\n",
                event, detail, (unsigned)s_cnt);
  return false;
}

// ------------------------------------------------------------ 出队补报
bool drainOne(PubSubClient &mqtt, const String &topic, const String &policyName) {
  if (s_cnt == 0 || !mqtt.connected()) return false;
  Item &it = s_q[s_head];
  bool ok = publishJson(mqtt, topic, it.event, it.detail, policyName, true, it.upMs, it.seq);
  if (!ok) return false;                     // 代理没收到，留在队头下次再试
  Serial.printf("[补报] #%lu %s (%s)  离线了 %lus 才发出去\n",
                (unsigned long)it.seq, it.event, it.detail,
                (unsigned long)((millis() - it.upMs) / 1000));
  s_head = (s_head + 1) % Q_CAP;
  s_cnt--;
  return true;
}

// ------------------------------------------------------------ 串口调试
void dump() {
  Serial.printf("  ┌── 离线事件队列 ────────────────\n");
  if (s_cnt == 0) {
    Serial.println("  │ （空）队列 0 条，累计被挤掉 0 条");
  } else {
    Serial.printf("  │ 排队 %u 条 / 容量 %u   累计挤掉 %lu 条\n",
                  (unsigned)s_cnt, Q_CAP, (unsigned long)s_dropped);
    for (uint8_t i = 0; i < s_cnt; i++) {
      Item &it = s_q[(s_head + i) % Q_CAP];
      Serial.printf("  │ #%lu  +%lus  %s (%s)\n",
                    (unsigned long)it.seq,
                    (unsigned long)(it.upMs / 1000), it.event, it.detail);
    }
  }
  Serial.println("  └────────────────────────────────");
}

} // namespace evq
