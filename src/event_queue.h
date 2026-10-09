// ============================================================
//  event_queue —— 触发事件离线补报队列（M1 · dev/v2）
//
//  解决的问题：publishEvent() 在 MQTT 断开时直接丢事件。
//  现在：连着就发（offline:false）；断着就进环形队列，
//        网络恢复后按时间顺序补报，带 offline:true + age_ms，
//        后台能区分"实时事件"和"补报事件"。
//
//  ★ M1 范围：队列在 RAM 里，只覆盖"同一次清醒窗口"内的离线。
//    深度睡眠会清空 —— 但事件日志 /events.csv（SD）是持久层，
//    跨睡眠的云端补报留给 M1.5（需要 NTP 真实时间戳）。
// ============================================================
#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

namespace evq {

void     begin();            // 清空队列、序号归零
// 入队/直发统一入口：MQTT 连着 -> 立即发(offline:false)；断着 -> 入队(offline:true)
// 返回 true = 已发布；false = 已入队（或队列满被挤掉）
bool     submit(PubSubClient &mqtt, const String &topic, const String &policyName,
                const char *event, const char *detail);

uint8_t  pending();          // 队列里还剩几条
uint32_t dropped();          // 累计被挤掉多少条（满 24 后挤最旧）
void     dump();             // 串口打印队列内容（调试用）

// 出队一条并发布。MQTT 必须已连接；没有可发的返回 false
bool     drainOne(PubSubClient &mqtt, const String &topic, const String &policyName);

} // namespace evq
