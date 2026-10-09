#include <Adafruit_SSD1306.h>
#include "oled_ui.h"
#include "config.h"

static Adafruit_SSD1306 s_disp(OLED_W, OLED_H, &Wire, -1);

void OledUi::begin()
{
    _ok = s_disp.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    if (!_ok) {
        Serial.println("[OLED] ✗ 0x3C 无应答 —— 查 SDA=8/SCL=7/供电；不影响其它功能");
        return;
    }
    Serial.printf("[OLED] ✓ %dx%d 就绪 @0x%02X\n", OLED_W, OLED_H, OLED_ADDR);
    idle(millis() / 1000);
}

void OledUi::idle(uint32_t upSec)
{
    if (!_ok) return;
    s_disp.clearDisplay();
    s_disp.setTextColor(SSD1306_WHITE);
    s_disp.setTextSize(1);
    s_disp.setCursor(0, 0);
    s_disp.println("Smart Sentinel");
    s_disp.setCursor(0, 12);
    s_disp.println("v2 + AI vision");
    char b[28];
    snprintf(b, sizeof(b), "up %02lu:%02lu:%02lu",
             (unsigned long)(upSec / 3600),
             (unsigned long)((upSec / 60) % 60),
             (unsigned long)(upSec % 60));
    s_disp.setCursor(0, 30);
    s_disp.println(b);
    s_disp.display();
    _onEvent = false;
}

void OledUi::aiResult(const char *animalEn, float conf, const char *timeStr)
{
    if (!_ok) return;
    s_disp.clearDisplay();
    s_disp.setTextColor(SSD1306_WHITE);
    s_disp.setTextSize(1);
    s_disp.setCursor(0, 0);
    s_disp.print("AI DETECT");
    s_disp.setCursor(78, 0);
    s_disp.print(timeStr);                        // NTP 未同步时是 --:--:--
    s_disp.setTextSize(2);                       // "wild_boar" 9字 x 12px = 108px，装得下
    s_disp.setCursor(0, 14);
    s_disp.println(animalEn);
    s_disp.setTextSize(1);
    char buf[24];
    snprintf(buf, sizeof(buf), "confidence %.0f%%", conf * 100.0f);
    s_disp.setCursor(0, 38);
    s_disp.print(buf);
    s_disp.drawRect(0, 52, OLED_W, 10, SSD1306_WHITE);
    int w = (int)(conf * (OLED_W - 2));
    if (w < 1) w = 1;
    if (w > OLED_W - 2) w = OLED_W - 2;
    s_disp.fillRect(1, 53, w, 8, SSD1306_WHITE);
    s_disp.display();
    _onEvent = true;
    _pageUntil = millis() + 6000;
}

void OledUi::tick()
{
    if (_ok && _onEvent && (int32_t)(millis() - _pageUntil) >= 0) idle(millis() / 1000);
}
