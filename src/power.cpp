#include "power.h"
#include "config.h"
#include <esp_sleep.h>

uint32_t Power::pirMaskAll() {
  return (1ULL << PIN_PIR_1) | (1ULL << PIN_PIR_2) | (1ULL << PIN_PIR_3);
}

// 这个变量放在 RTC 内存里，深度睡眠不会丢
RTC_DATA_ATTR static bool s_inCooldown = false;

const char *Power::causeText() const {
  switch (_cause) {
    case WAKE_PIR:      return "PIR 触发";
    case WAKE_TIMER:    return "定时同步";
    case WAKE_COOLDOWN: return "冷却结束";
    case WAKE_COLD:     return "上电/复位";
    default:            return "其它";
  }
}

uint32_t Power::pirWakeMask() const { return _pirMask; }

void Power::begin() {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      _cause = WAKE_PIR;
      _pirMask = (uint32_t)esp_sleep_get_ext1_wakeup_status() & pirMaskAll();
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
      if (s_inCooldown) {
        s_inCooldown = false;
        _cause = WAKE_COOLDOWN;      // 冷却睡够了，该回去正常睡
      } else {
        _cause = WAKE_TIMER;
      }
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      _cause = WAKE_COLD;
      break;
    default:
      _cause = WAKE_OTHER;
      break;
  }
}

void Power::printWakeInfo() const {
  Serial.printf("[电源] 唤醒原因: %s", causeText());
  if (_cause == WAKE_PIR) {
    Serial.print("  触发脚: ");
    if (_pirMask & (1ULL << PIN_PIR_1)) Serial.print("PIR1 ");
    if (_pirMask & (1ULL << PIN_PIR_2)) Serial.print("PIR2 ");
    if (_pirMask & (1ULL << PIN_PIR_3)) Serial.print("PIR3 ");
  }
  Serial.println();
  Serial.printf("[电源] 深度睡眠: %s   PIR 唤醒脚 GPIO%d/%d/%d\n",
                _sleepEnabled ? "已启用" : "已关闭(调试模式)",
                PIN_PIR_1, PIN_PIR_2, PIN_PIR_3);
}

void Power::enterDeepSleep(uint64_t seconds) {
  s_inCooldown = false;
  Serial.printf("[睡眠] 进入深度睡眠。%s\n",
                seconds ? "到点自动醒来同步策略" : "只等 PIR 触发");
  Serial.flush();

  esp_sleep_enable_ext1_wakeup(pirMaskAll(), ESP_EXT1_WAKEUP_ANY_HIGH);
  if (seconds > 0) {
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  }
  esp_deep_sleep_start();     // 不返回
}

void Power::enterCooldownSleep(uint64_t seconds) {
  s_inCooldown = true;
  Serial.printf("[睡眠] 进入冷却期 %llu 秒（PIR 唤醒已关闭，动物赖着不走也不会把电耗光）\n",
                (unsigned long long)seconds);
  Serial.flush();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();     // 不返回
}