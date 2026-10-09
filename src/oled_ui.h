#pragma once
#include <Arduino.h>
#include <Wire.h>

// ============================================================
//  V2 · OLED 状态屏（SSD1306 128x64 I2C 0x3C，与 AHT10 共用 SDA=8/SCL=7）
//  没接屏 / 型号不同 -> begin() 失败，ok()=false，所有调用自动跳过，
//  不影响触发/播放/上报等 v1 功能。
//  显示英文键 + 置信度条：默认字库无中文，label_cn 走 MQTT/事件日志。
// ============================================================
class OledUi {
public:
    void begin();
    bool ok() const { return _ok; }
    // 识别结果页：动物名(大字) + 置信度% + 时间，约 6s 后自动回待机页
    void aiResult(const char *animalEn, float conf, const char *timeStr);
    void idle(uint32_t upSec);                        // 待机页：系统名 + 运行时长
    void tick();                                      // 主循环推动：结果页超时回待机

private:
    bool     _ok = false;
    bool     _onEvent = false;
    uint32_t _pageUntil = 0;
};
