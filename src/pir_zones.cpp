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

// ============================================================
//  V2.1 接线诊断：ESP32 自己当万用表
//    ① 全线路去拉 → 三态判定 + ADC 电压：判出 悬空 / 有电空闲 / 没电钳位 / 正在输出
//    ② 线互短测试：A 上拉 B 读高 = 两根线短在一起
//    ③ 实时边沿监听：挥手、碰线，当场打印每一次电平变化
// ============================================================

static void IRAM_ATTR monISR0() { if (g_pir) g_pir->edgeEvent(0, digitalRead(PIR_PIN_MAP[0])); }
static void IRAM_ATTR monISR1() { if (g_pir) g_pir->edgeEvent(1, digitalRead(PIR_PIN_MAP[1])); }
static void IRAM_ATTR monISR2() { if (g_pir) g_pir->edgeEvent(2, digitalRead(PIR_PIN_MAP[2])); }

static void (*const ISR_TAB[PirZones::COUNT])() = { pirISR0, pirISR1, pirISR2 };
static void (*const MON_TAB[PirZones::COUNT])() = { monISR0, monISR1, monISR2 };

// 4 次取平均，滤掉单次 ADC 抖动；在 core 0 任务里跑，绕开 ipc1 任务栈溢出
//   （实测：setup 里在 core 1 直接调 analogReadMilliVolts 偶发 Stack canary 崩溃）
static volatile bool g_adcBusy = false;
static volatile int  g_adcRes  = 0;
static volatile uint8_t g_adcPin = 0;

static void adcReadTask(void *) {
  uint32_t acc = 0;
  for (int i = 0; i < 4; i++) { acc += (uint32_t)analogReadMilliVolts(g_adcPin); delay(2); }
  g_adcRes  = (int)(acc >> 2);
  g_adcBusy = false;
  vTaskDelete(NULL);
}

static int pinMv(uint8_t pin) {
  g_adcPin = pin; g_adcRes = 0; g_adcBusy = true;
  if (xTaskCreatePinnedToCore(adcReadTask, "adc0", 6144, NULL, 5, NULL, 0) != pdPASS)
    return -1;
  uint32_t t0 = millis();
  while (g_adcBusy && (millis() - t0 < 500)) delay(2);
  return g_adcRes;
}

void PirZones::lineDiag(bool withAdc) {
  Serial.println();
  Serial.println("[PIR] ▼ 线路诊断（全线路去拉 → 三态判定 + ADC 电压）");
  Serial.println("[PIR]   判读: 悬空=OUT没接到有电的输出 / 硬0V=有电模块空闲待机 /");
  Serial.println("[PIR]         钳位0.5~0.9V=该列挂了【没电的模块】★ / ~3.1V=正在输出(自触发)✓");

  // —— 第一步：三根线全部变成无拉纯输入 ——
  //    （消除其它脚内部下拉的干扰：否则线互短时会读出假中间电压）
  for (uint8_t i = 0; i < COUNT; i++) {
    detachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]));
    pinMode(PIR_PIN_MAP[i], INPUT);
  }
  delay(5);

  int     mv0[COUNT], mv1[COUNT];
  uint8_t r0[COUNT],  r1[COUNT];

  // —— 第二步：逐线测裸电平（其它线全无拉），再仅给被测线上拉测一次 ——
  for (uint8_t i = 0; i < COUNT; i++) {
    uint8_t p = PIR_PIN_MAP[i];
    r0[i] = digitalRead(p);
    mv0[i] = withAdc ? pinMv(p) : -1;

    pinMode(p, INPUT_PULLUP);            // 只有被测线自己上拉，其它线仍然无拉
    delay(4);
    r1[i] = digitalRead(p);
    mv1[i] = withAdc ? pinMv(p) : -1;

    pinMode(p, INPUT);
  }

  // —— 第三步：线互短测试（A 上拉 → B 读高 = 两根线连通）——
  uint8_t tie[COUNT][COUNT] = {0};
  for (uint8_t a = 0; a < COUNT; a++) {
    pinMode(PIR_PIN_MAP[a], INPUT_PULLUP);
    delay(3);
    for (uint8_t b = 0; b < COUNT; b++) {
      if (b == a) continue;
      tie[a][b] = digitalRead(PIR_PIN_MAP[b]);   // B 无拉读到高 → 与 A 连通
    }
    pinMode(PIR_PIN_MAP[a], INPUT);
  }

  // —— 第四步：恢复正常配置（下拉 + 上升沿中断）——
  for (uint8_t i = 0; i < COUNT; i++) {
    pinMode(PIR_PIN_MAP[i], INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]), ISR_TAB[i], RISING);
  }

  // —— 判读 ——
  for (uint8_t i = 0; i < COUNT; i++) {
    const char *verdict;
    if (withAdc) {
      if      (mv0[i] > 2500)               verdict = "正在输出高电平（自触发/检测中）—— 有电且链路通 ✓";
      else if (mv0[i] < 250)                verdict = "硬拉到 0V：有电模块空闲待机（正常）；或线对地短路";
      else if (mv0[i] <= 900)               verdict = "钳位 0.xV：该列挂了【没电的模块】★头号嫌疑（断电模块OUT会钳线）";
      else if (mv1[i] > 2600)               verdict = "悬空：上拉才抬起来——OUT 没接到任何有电的输出 ★";
      else                                  verdict = "中间电平持续：外部弱下拉/接触半通，人工排查";
    } else {
      if      (r0[i] == HIGH)               verdict = "当前高电平：模块正在输出";
      else if (r1[i] == LOW)                verdict = "被驱动为低：模块有电且空闲（正常待机）";
      else if (r1[i] == HIGH)               verdict = "悬空：上拉才为高——OUT 没接到有电的输出 ★";
      else                                  verdict = "无法判定";
    }

    if (withAdc)
      Serial.printf("[PIR] %s GPIO%-2d  裸测 %4dmV  仅己上拉 %4dmV  → %s\n",
                    PIR_NAME_MAP[i], PIR_PIN_MAP[i], mv0[i], mv1[i], verdict);
    else
      Serial.printf("[PIR] %s GPIO%-2d  裸 %d 上拉 %d  → %s\n",
                    PIR_NAME_MAP[i], PIR_PIN_MAP[i], r0[i], r1[i], verdict);
  }

  // —— 互短结果 ——
  bool anyTie = false;
  for (uint8_t a = 0; a < COUNT; a++)
    for (uint8_t b = a + 1; b < COUNT; b++)
      if (tie[a][b] || tie[b][a]) {
        anyTie = true;
        Serial.printf("[PIR] ⚡ 线互短：GPIO%d 和 GPIO%d 的两根线短在一起（面包板同列/同一孔）！\n",
                      PIR_PIN_MAP[a], PIR_PIN_MAP[b]);
      }
  if (!anyTie) Serial.println("[PIR] 线互短测试：三根线彼此独立（没拧在一起）");

  Serial.println("[PIR] ▲ 诊断结束，对号入座定位断点");
  Serial.println();
}

void IRAM_ATTR PirZones::edgeEvent(uint8_t zone, uint8_t level) {
  if (level) raise(zone);                       // 上升沿照常走触发逻辑，监听期功能不变
  if (!_monUntil) return;
  uint8_t w    = _ew;
  uint8_t next = (uint8_t)((w + 1) % 48);
  if (next == _er) { _dropped++; return; }      // 缓冲满：折叠，不阻塞中断
  _eb[w].t = millis(); _eb[w].zone = zone; _eb[w].lvl = level;
  _ew = next;
}

void PirZones::beginMonitor(uint32_t ms) {
  noInterrupts(); _ew = _er = 0; _dropped = 0; interrupts();
  _monUntil = millis() + ms;
  for (uint8_t i = 0; i < COUNT; i++) {
    detachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]));
    attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]), MON_TAB[i], CHANGE);
  }
  Serial.printf("[监听] ★ 开始 %lus 秒实时边沿监听：每根线每次电平变化都立刻打印\n",
                (unsigned long)(ms / 1000));
  Serial.println("[监听]   现在去挥手/碰线，哪根线活着当场见分晓");
}

void PirZones::monitorTick() {
  if (!_monUntil) return;

  if ((int32_t)(millis() - _monUntil) >= 0) {   // 时段到：恢复上升沿触发
    for (uint8_t i = 0; i < COUNT; i++) {
      detachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]));
      attachInterrupt(digitalPinToInterrupt(PIR_PIN_MAP[i]), ISR_TAB[i], RISING);
    }
    _monUntil = 0;
    Serial.println("[监听] ○ 时段结束，已恢复上升沿触发");
    return;
  }

  // 搬运 + 打印新边沿（每轮最多 8 条，防刷爆）
  EdgeRec out[8];
  uint8_t n = 0;
  uint16_t dropped;
  noInterrupts();
  while (_er != _ew && n < 8) {
    out[n++] = _eb[_er];
    _er = (uint8_t)((_er + 1) % 48);
  }
  dropped = _dropped; _dropped = 0;
  interrupts();

  for (uint8_t k = 0; k < n; k++) {
    Serial.printf("[监听] +%lu.%02lus  %s GPIO%-2d %s\n",
                  (unsigned long)(out[k].t / 1000),
                  (unsigned long)((out[k].t % 1000) / 10),
                  PIR_NAME_MAP[out[k].zone], PIR_PIN_MAP[out[k].zone],
                  out[k].lvl ? "▲上升" : "▼下降");
    // 不同分区 20ms 内同跳 → 线互短的可疑特征
    for (uint8_t j = (uint8_t)(k + 1); j < n; j++) {
      if (out[j].zone != out[k].zone && out[j].lvl == out[k].lvl &&
          (int32_t)(out[j].t - out[k].t) >= 0 && (out[j].t - out[k].t) < 20)
        Serial.println("[监听] ⚡ 两个分区同刻跳变 —— 疑似两根线短在一起");
    }
  }
  if (dropped) Serial.printf("[监听] …边沿太密，另有 %u 条被折叠（噪声/抖动）\n", dropped);
}
