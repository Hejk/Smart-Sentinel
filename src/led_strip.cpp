#include "led_strip.h"
#include <Adafruit_NeoPixel.h>

bool LedStrip::begin() {
  if (_px) return true;
  _px = new Adafruit_NeoPixel(RGB_COUNT, PIN_RGB_DIN, NEO_GRB + NEO_KHZ800);
  if (!_px) {
    Serial.println("[RGB] 内存不足，灯带不可用");
    return false;
  }
  _px->begin();
  _px->setBrightness(_bright);
  _px->clear();
  _px->show();
  Serial.printf("[RGB] WS2812 %d 颗就绪  DIN=GPIO%d  亮度=%d\n",
                RGB_COUNT, PIN_RGB_DIN, _bright);
  return true;
}

void LedStrip::setBrightness(uint8_t b) {
  _bright = b;
  if (_px) {
    _px->setBrightness(b);
    _px->show();
  }
}

void LedStrip::off() {
  if (!_px) return;
  _px->clear();
  _px->show();
}

void LedStrip::fill(uint8_t r, uint8_t g, uint8_t b) {
  if (!_px) return;
  for (int i = 0; i < RGB_COUNT; i++) _px->setPixelColor(i, _px->Color(r, g, b));
  _px->show();
}

void LedStrip::blip(uint8_t r, uint8_t g, uint8_t b, uint16_t ms) {
  if (!_px) return;
  fill(r, g, b);
  delay(ms);
  off();
}

void LedStrip::strobe(uint8_t r, uint8_t g, uint8_t b,
                      uint8_t times, uint16_t onMs, uint16_t offMs) {
  if (!_px) return;
  for (uint8_t i = 0; i < times; i++) {
    fill(r, g, b);
    delay(onMs);
    off();
    delay(offMs);
  }
}

void LedStrip::bootChase() {
  if (!_px) return;
  for (int i = 0; i < RGB_COUNT; i++) {
    _px->clear();
    _px->setPixelColor(i, _px->Color(0, 0, 80));
    _px->show();
    delay(60);
  }
  off();
}