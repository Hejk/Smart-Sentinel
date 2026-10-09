#include "audio_out.h"

#include <math.h>
#include <string.h>
#include <driver/i2s.h>
#include <esp_idf_version.h>

// 说明：这里用的是 ESP-IDF 的 legacy I2S 驱动 (driver/i2s.h)。
// 它在 IDF 4.4 (Arduino core 2.x) 和 IDF 5.x (Arduino core 3.x) 上都还在，
// 只是 5.x 会带一条 deprecated 警告，属于预期内的。
// 之所以不用 ESP8266Audio / ESP32-audioI2S 这类库：它们的新版本都指向
// IDF5 的 driver/i2s_std.h，与本项目的 core 2.0.17 不兼容。

static inline void i2sWriteAll(const int16_t *buf, size_t samples) {
  size_t bytes = samples * sizeof(int16_t);
  size_t done = 0;
  while (done < bytes) {
    size_t bw = 0;
    if (i2s_write(I2S_NUM_0, (const char *)buf + done, bytes - done, &bw, portMAX_DELAY) != ESP_OK) break;
    if (bw == 0) break;
    done += bw;
  }
}

bool AudioOut::begin() {
  if (_ready) return true;

  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = _rate;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;   // 单声道走左声道，MAX98357 默认 (L+R)/2
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;   // 标准 I2S，配合 MAX98357
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] i2s_driver_install 失败: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  pins.mck_io_num = I2S_PIN_NO_CHANGE;   // IDF5 新增字段，不接主时钟
#endif
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_LRC;
  pins.data_out_num = PIN_I2S_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) {
    Serial.printf("[I2S] i2s_set_pin 失败: %d\n", (int)err);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  _ready = true;
  Serial.printf("[I2S] 就绪  BCLK=%d LRC=%d DIN=%d  %luHz/16bit 单声道\n",
                PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN, (unsigned long)_rate);
  return true;
}

void AudioOut::end() {
  if (!_ready) return;
  flush();
  i2s_driver_uninstall(I2S_NUM_0);
  _ready = false;
}

void AudioOut::setVolume(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  _vol = v;
}

bool AudioOut::setSampleRate(uint32_t hz) {
  if (hz < 8000)  hz = 8000;
  if (hz > 96000) hz = 96000;
  if (hz == _rate) return true;
  if (!_ready) { _rate = hz; return false; }

  esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, hz);
  if (err != ESP_OK) {
    Serial.printf("[I2S] 切换采样率到 %luHz 失败: %d\n", (unsigned long)hz, (int)err);
    return false;
  }
  _rate = hz;
  return true;
}

bool AudioOut::write(const int16_t *samples, size_t count) {
  if (!_ready || count == 0) return false;
  i2sWriteAll(samples, count);
  return true;
}

void AudioOut::tone(uint16_t freq, uint16_t ms) {
  if (!_ready) return;
  if (_rate != SR_DEFAULT) setSampleRate(SR_DEFAULT);   // 音频流可能改过采样率，切回来

  const int32_t total = (int32_t)((uint64_t)_rate * ms / 1000);
  const int32_t fade  = 220;                 // 淡入淡出样本数，避免咔哒声
  const float   amp   = 0.32f * _vol;        // 留够余量，3W 喇叭别一上来就爆音

  static int16_t buf[512];
  int32_t i = 0;
  while (i < total) {
    int32_t n = 0;
    while (n < 512 && i + n < total) {
      int32_t k = i + n;
      float env = 1.0f;
      if (k < fade)                 env = (float)k / fade;
      else if (k > total - fade)    env = (float)(total - k) / fade;
      float s = sinf(2.0f * PI * freq * k / _rate) * env * amp;
      buf[n++] = (int16_t)(s * 32767.0f);
    }
    i2sWriteAll(buf, n);
    i += n;
  }
  flush();
}

void AudioOut::chime() {
  tone(880, 120);
  tone(1318, 160);
}

void AudioOut::flush() {
  if (!_ready) return;
  static int16_t zero[512] = {0};
  // 写两轮静音，把 DMA 里剩余的音频推出去
  i2sWriteAll(zero, 512);
  i2sWriteAll(zero, 512);
}