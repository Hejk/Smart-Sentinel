#include "pir_zones.h"

static PirZones *g_pir = nullptr;

static void IRAM_ATTR pirISR0() { if (g_pir) g_pir->raise(0); }
static void IRAM_ATTR pirISR1() { if (g_pir) g_pir->raise(1); }
static void IRAM_ATTR pirISR2() { if (g_pir) g_pir->raise(2); }

static const uint8_t PIR_PIN_MAP[PirZones::COUNT] = { PIN_PIR_1, PIN_PIR_2, PIN_PIR_3 };
static const char   *PIR_NAME_MAP[PirZones::COUNT] = { "1号", "2号", "3号" };

uint8_t PirZones::pin(uint8_t zone) {
  return (zone < COUNT) ? PIR_PIN_MAP[zone] : 0xFF;
}

const char *PirZones::name(uint8_t zone) {
  return (zone < COUNT) ? PIR_NAME_MAP[zone] : "?";
}

bool PirZones::rawActive(uint8_t zone) const {
  if (zone >= COUNT) return false;
  return digitalRead(PIR_PIN_MAP[zone]) == HIGH;
}

bool PirZones::warmupDone() const {
  return (int32_t)(millis() - _readyAt) >= 0;
}

void PirZones::begin(bool needWarmup) {
  g_pir = this;
  _flags = 0;

  for (uint8_t i = 0; i < COUNT; i++) {
    // HC-SR501 推挽输出，但没上电时引脚会飘；内部下拉能防止误触发
    pinMode(PIR_PIN_MAP[i], INPUT_PULLDOWN);
  }

  attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[0]), pirISR0, RISING);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[1]), pirISR1, RISING);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[2]), pirISR2, RISING);

  _readyAt = needWarmup ? (millis() + PIR_WARMUP_MS) : millis();

  Serial.printf("[PIR] 已挂载 3 路（上升沿触发）: ");
  for (uint8_t i = 0; i < COUNT; i++)
    Serial.printf("%s=GPIO%d  ", PIR_NAME_MAP[i], PIR_PIN_MAP[i]);
  Serial.println();
  if (needWarmup) {
    Serial.printf("[PIR] 冷启动，等待 %lu 秒预热（期间触发被忽略）\n",
                  (unsigned long)(PIR_WARMUP_MS / 1000));
  } else {
    Serial.println("[PIR] 深睡唤醒：PIR 一直通电，无需预热，立即可用");
  }
}

int PirZones::pop() {
  noInterrupts();
  uint8_t f = _flags;
  interrupts();
  if (!f) return -1;

  for (uint8_t i = 0; i < COUNT; i++) {
    if (f & (uint8_t)(1 << i)) {
      noInterrupts();
      _flags &= (uint8_t)~(1 << i);
      interrupts();
      return i;
    }
  }
  return -1;
}