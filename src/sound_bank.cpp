#include "sound_bank.h"

#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include "config.h"

// ---------------------------------------------------------------- 挂载
bool SoundBank::mountSD() {
  Serial.printf("[SD] 挂载 TF 卡  CS=%d SCK=%d MISO=%d MOSI=%d ...\n",
                PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);
  // ★ 排序原则：上电后第一只碰卡的必须是 400kHz 规范内初始化。
  //   SD 卡规范：初始化阶段（CMD0~ACMD41）要求 ≤400kHz；而且初始化失败
  //   的卡必须断电才能重来（软复位无效）。旧顺序是 SD 库先用 4MHz 硬上，
  //   失败后卡卡死在 idle，之后谁也救不活 —— 诊断证据：CMD0/8 全对、
  //   ACMD41 卡 40 轮不就绪。
  _sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  SD.end();

  // ① 400kHz 手动预初始化（首次带诊断日志）—— 把卡拉到“就绪”
  bool warm = sdWarmup(!_initTraced);
  _initTraced = true;

  // ② 就绪后 4MHz 正常挂载
  if (warm && SD.begin(PIN_SD_CS, _sdSPI, 4000000)) return finishMount();
  if (warm) {
    SD.end();
    // ③ 退到 400kHz 挂载（读写慢，但先把功能跑通）
    if (SD.begin(PIN_SD_CS, _sdSPI, 400000)) {
      Serial.println("[SD] ✓ 400kHz 慢速挂载成功（读速受限，音频流可能卡顿）");
      return finishMount();
    }
    SD.end();
    Serial.println("[SD] ✗ 挂载失败：卡有应答但读不出文件系统 —— 查卡是不是 FAT32");
    sdProbe();
    return false;
  }

  // ★ 预热失败（卡没有标准应答）时不再进 SD 库的重试路径：
  //   对“半死不活”的卡，SD.begin 内部的 f_mount 会永久卡死且无任何
  //   输出（实测）。直接裸探测定位原因后放弃本轮 —— 10 秒自动重试
  //   会再进来；卡断电重启 / 插紧 / 线纠正后，预热成功即恢复。
  SD.end();
  Serial.println("[SD] ✗ 挂载失败：卡没完成初始化握手（断电重来 / 查接线 / 查供电）");
  sdProbe();                     // 硬件级诊断：看卡在 SPI 总线上有没有应答
  return false;
}

bool SoundBank::finishMount() {
  uint8_t t = SD.cardType();
  if (t == CARD_NONE) {
    Serial.println("[SD] ✗ 没检测到卡");
    return false;
  }
  const char *tn = (t == CARD_MMC) ? "MMC" : (t == CARD_SD) ? "SDSC"
                 : (t == CARD_SDHC) ? "SDHC" : "未知";
  Serial.printf("[SD] ✓ %s 卡  %llu MB  路径前缀 /sd\n",
                tn, SD.cardSize() / (1024ULL * 1024ULL));
  return true;
}

// ---------------------------------------------------------------- 手动全握手
// 400kHz 手动跑一遍 SD 初始化序列（CMD0 → CMD8 → ACMD41 循环 → CMD58）。
// 动机：SD 库挂载失败但卡应答 CMD0 时，必须知道卡到底卡在哪一步。
// verbose=true 时逐步打印应答字节（诊断报告），false 时静默（10 秒重试用）。
// 返回 true = 卡完成 SPI 初始化，可立刻再试高速挂载。
bool SoundBank::sdWarmup(bool verbose) {
  auto sendCmd = [&](uint8_t idx, uint32_t arg, uint8_t crc) {
    uint8_t f[6] = { (uint8_t)(idx | 0x40), (uint8_t)(arg >> 24),
                     (uint8_t)(arg >> 16), (uint8_t)(arg >> 8),
                     (uint8_t)arg, crc };
    for (int i = 0; i < 6; i++) _sdSPI.transfer(f[i]);
  };
  auto readR1 = [&]() {          // 等 R1（首字节非 0xFF 即应答）
    uint8_t r = 0xFF;
    for (int t = 0; t < 16 && r == 0xFF; t++) r = _sdSPI.transfer(0xFF);
    return r;
  };
  auto eat = [&](int n) { for (int i = 0; i < n; i++) _sdSPI.transfer(0xFF); };

  if (verbose) Serial.println("[SD] ▼ 全握手追踪（400kHz 手动初始化）：");
  pinMode(PIN_SD_CS, OUTPUT);
  _sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  // 上电哑时钟：80 个（CS 高）
  digitalWrite(PIN_SD_CS, HIGH);
  for (int i = 0; i < 10; i++) _sdSPI.transfer(0xFF);

  // ---- CMD0 软复位，期望 R1=0x01（idle）
  digitalWrite(PIN_SD_CS, LOW);
  sendCmd(0, 0, 0x95);
  uint8_t r1 = readR1(); eat(6);
  digitalWrite(PIN_SD_CS, HIGH);
  if (verbose) Serial.printf("      CMD0   → R1=%02X  %s\n", r1,
                r1 == 0x01 ? "(idle ✓)" : "(异常)");
  if (r1 != 0x01) { _sdSPI.endTransaction(); return false; }

  // ---- CMD8 电压探测：2.0 卡应回显 01 AA
  digitalWrite(PIN_SD_CS, LOW);
  sendCmd(8, 0x000001AA, 0x87);
  uint8_t r8 = readR1();
  uint8_t echo[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < 4; i++) echo[i] = _sdSPI.transfer(0xFF);
  eat(6);
  digitalWrite(PIN_SD_CS, HIGH);
  bool v2 = (r8 == 0x01) && (echo[2] == 0x01) && (echo[3] == 0xAA);
  if (verbose) Serial.printf("      CMD8   → R1=%02X 回显=%02X%02X%02X%02X  %s\n",
                r8, echo[0], echo[1], echo[2], echo[3],
                v2 ? "(v2/SDHC 卡 ✓)" : "(按 v1 卡处理)");

  // ---- ACMD41 初始化循环：等 R1=0x00（就绪）。
  //   ★ 参数必须带主机电压窗口（0x00FF8000 = 支持 2.7~3.6V）。
  //   监控/高耐久级的卡对这条严格执行：不声明电压窗口 → 永不就绪，
  //   症状与 CMD0/CMD8 正常但 ACMD41 卡死完全一致（spec 赋予卡的权利）。
  //   v2 卡再叠加 HCS 位（bit31），v1 卡不带 HCS。两种都试。
  uint32_t args41[2] = { v2 ? 0x40FF8000u : 0x00FF8000u, 0x00FF8000u };
  uint8_t  r41 = 0xFF;
  int      rounds = 0;
  uint32_t spent = 0;
  for (int mode = 0; mode < 2 && r41 != 0x00; mode++) {
    uint32_t arg41 = args41[mode];
    if (mode == 1) {          // 带窗口参数失败才试无 HCS 版本
      if (!v2) break;
      if (verbose) Serial.println("      · 去掉 HCS 位重试 ACMD41 ...");
    }
    uint32_t t0 = millis();
    rounds = 0;
    while (millis() - t0 < 2000) {
      digitalWrite(PIN_SD_CS, LOW);
      sendCmd(55, 0, 0x01);
      uint8_t r55 = readR1();
      if (verbose && rounds == 0)
        Serial.printf("      CMD55  → R1=%02X  %s\n", r55,
                      (r55 == 0x01 || r55 == 0x00) ? "(初始化期应答正常 ✓)" : "(应答异常)");
      eat(4);
      sendCmd(41, arg41, 0x01);
      r41 = readR1(); eat(4);
      digitalWrite(PIN_SD_CS, HIGH);
      rounds++;
      if (r41 == 0x00) break;
      delay(50);
    }
    spent = millis() - t0;
  }
  if (verbose) Serial.printf("      ACMD41 → R1=%02X  %s (%d 轮, %lu ms)\n",
                r41, r41 == 0x00 ? "(就绪 ✓)" : "✗ 始终未就绪（卡需要断电重试/供电不足）",
                rounds, (unsigned long)spent);
  if (r41 != 0x00) { _sdSPI.endTransaction(); return false; }

  // ---- CMD58 读 OCR（电压窗口 + 容量类别）
  digitalWrite(PIN_SD_CS, LOW);
  sendCmd(58, 0, 0x01);
  uint8_t r58 = readR1();
  uint8_t ocr[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < 4; i++) ocr[i] = _sdSPI.transfer(0xFF);
  eat(6);
  digitalWrite(PIN_SD_CS, HIGH);
  bool ccs = ocr[0] & 0x40;                 // bit30 → SDHC/SDXC 块寻址
  if (verbose) Serial.printf("      CMD58  → R1=%02X OCR=%02X%02X%02X%02X  %s\n",
                r58, ocr[0], ocr[1], ocr[2], ocr[3],
                ccs ? "(SDHC/SDXC 块寻址)" : "(字节寻址/小卡)");
  _sdSPI.endTransaction();
  return true;
}

// ---------------------------------------------------------------- SPI 裸探测
// 绕过 SD 库直接问卡：拉低 CS 发 CMD0(复位)，看 MISO 返回什么。
// 全 0x00 → MISO 死低：卡没供电/没插好/槽坏/线不通
// 全 0xFF → 总线悬空：卡没应答（卡坏或 MISO 断）
// 出现 0x01 → 卡活着！(SD 库挂载才是问题)
void SoundBank::sdProbe() {
  SPISettings cfg(400000, MSBFIRST, SPI_MODE0);
  _sdSPI.beginTransaction(cfg);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  delayMicroseconds(10);

  Serial.println("[SD] ▼ SPI 裸探测（CS拉低后听 MISO）：");

  // 1) 总线闲时听 4 字节
  digitalWrite(PIN_SD_CS, LOW);
  delayMicroseconds(10);
  uint8_t idle[4];
  for (int i = 0; i < 4; i++) idle[i] = _sdSPI.transfer(0xFF);
  digitalWrite(PIN_SD_CS, HIGH);
  Serial.printf("      闲时读: %02X %02X %02X %02X\n", idle[0], idle[1], idle[2], idle[3]);

  // 2) 发 CMD0 复位命令，读最多 10 字节等应答
  digitalWrite(PIN_SD_CS, LOW);
  static const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
  for (int i = 0; i < 6; i++) _sdSPI.transfer(cmd0[i]);
  uint8_t resp = 0xFF;
  int tries = 0;
  while (resp == 0xFF && tries++ < 10) resp = _sdSPI.transfer(0xFF);
  // 再吐几个字节看后续
  uint8_t tail[4];
  for (int i = 0; i < 4; i++) tail[i] = _sdSPI.transfer(0xFF);
  digitalWrite(PIN_SD_CS, HIGH);
  _sdSPI.endTransaction();

  Serial.printf("      CMD0 应答: %02X  尾随: %02X %02X %02X %02X\n", resp, tail[0], tail[1], tail[2], tail[3]);

  if (resp == 0x01) {
    Serial.println("      ★ 卡是活的！SD 库挂载失败另有原因（往往是速度/CS）");
  } else if (resp == 0xFF && idle[0] == 0xFF && idle[1] == 0xFF) {
    Serial.println("      ✗ MISO 一直悬空高：卡没应答（卡坏/槽坏/未插到位）");
  } else if (resp == 0xFF && idle[0] == 0x00 && idle[1] == 0x00) {
    Serial.println("      ✗ MISO 死低：卡没真正接到总线（供电/接地/插槽）");
  } else {
    Serial.println("      ? 有波动但无标准应答，再看一眼接线");
  }
}

bool SoundBank::mountFS() {
  Serial.println("[Flash] 挂载板载 LittleFS ...");
  if (!LittleFS.begin(true)) {
    Serial.println("[Flash] ✗ 挂载失败");
    return false;
  }
  Serial.printf("[Flash] ✓ 已挂载  %u / %u 字节\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}

// ---------------------------------------------------------------- 目录扫描
void SoundBank::addFile(const String &name) {
  if (_count >= MAX_SOUNDS) return;
  _names[_count++] = name;
}

int SoundBank::indexOf(const String &name) const {
  for (int i = 0; i < _count; i++)
    if (_names[i] == name) return i;
  return -1;
}

void SoundBank::scan() {
  _count = 0;
  _last = -1;

  File dir = _fs->open(_root);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[音频库] ⚠ 目录 %s 不存在\n", _root.c_str());
    Serial.printf("[音频库]   请在卡里建 %s 目录，把 wav 放进去\n", _root.c_str());
    return;
  }

  File f = dir.openNextFile();
  while (f && _count < MAX_SOUNDS) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);      // 只留文件名
    if (!f.isDirectory() && (n.endsWith(".wav") || n.endsWith(".WAV"))) addFile(n);
    f = dir.openNextFile();
  }
  dir.close();
}

bool SoundBank::begin() {
  // 1) 优先 TF 卡
  if (mountSD()) {
    _sdOk = true;
    _fs = &SD;
    _root = "/sd/sounds";
    _backend = "TF 卡";
  } else if (mountFS()) {
    _fs = &LittleFS;
    _root = "/sounds";
    _backend = "板载 Flash";
  } else {
    Serial.println("[音频库] ✗ 两个后端都挂载失败，本地音频不可用");
    return false;
  }

  scan();
  Serial.printf("[音频库] 来源=%s  目录=%s  共 %d 条\n",
                _backend, _root.c_str(), _count);
  if (_count == 0) {
    Serial.println("[音频库] ⚠ 没有找到 wav，将只能靠云端音频或提示音");
  } else {
    for (int i = 0; i < _count; i++) Serial.printf("        %2d) %s\n", i, _names[i].c_str());
  }
  return true;
}

bool SoundBank::retrySD() {
  if (_sdOk) return false;
  Serial.println("[SD] ▲ 重试挂载 TF 卡 ...");
  if (!mountSD()) return false;
  _sdOk = true;
  _fs = &SD;
  _root = "/sd/sounds";
  _backend = "TF 卡";
  scan();
  Serial.printf("[SD] ✓ 重试成功！音频库已切换到 TF 卡  共 %d 条\n", _count);
  for (int i = 0; i < _count; i++) Serial.printf("        %2d) %s\n", i, _names[i].c_str());
  return true;
}

// ---------------------------------------------------------------- 查询
void SoundBank::printAll() const {
  Serial.printf("  ┌── 音频库 [%s] %s  (%d 条) ──\n", _backend, _root.c_str(), _count);
  for (int i = 0; i < _count; i++) {
    File f = _fs->open(_root + "/" + _names[i], "r");
    uint32_t sz = f ? f.size() : 0;
    if (f) f.close();
    Serial.printf("  │ %2d) %-22s %7u 字节\n", i, _names[i].c_str(), (unsigned)sz);
  }
  if (_count == 0) Serial.println("  │ (空)");
  Serial.println("  └──────────────────────────────────────");
}

File SoundBank::open(int idx) {
  if (!_fs || idx < 0 || idx >= _count) return File();
  return _fs->open(_root + "/" + _names[idx], "r");
}

// ---------------------------------------------------------------- 挑选
int SoundBank::pick(const String &preferList, bool randomPick) {
  if (_count == 0) return -1;

  int cand[MAX_SOUNDS];
  int n = 0;

  if (preferList.length() == 0) {
    for (int i = 0; i < _count && n < MAX_SOUNDS; i++) cand[n++] = i;
  } else {
    int start = 0;
    while (start < (int)preferList.length() && n < MAX_SOUNDS) {
      int comma = preferList.indexOf(',', start);
      String s = (comma < 0) ? preferList.substring(start) : preferList.substring(start, comma);
      s.trim();
      if (s.length()) {
        int idx = indexOf(s);
        if (idx >= 0) cand[n++] = idx;
        else Serial.printf("[音频库] 策略指定的 \"%s\" 不存在，忽略\n", s.c_str());
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    if (n == 0) {
      Serial.println("[音频库] 策略给的音频名一个都不存在 -> 回退到全部");
      for (int i = 0; i < _count && n < MAX_SOUNDS; i++) cand[n++] = i;
    }
  }
  if (n == 0) return -1;
  if (n == 1) {
    _last = cand[0];
    return cand[0];
  }

  int chosen = cand[0];
  if (randomPick) {
    for (int tries = 0; tries < 8; tries++) {
      chosen = cand[random(n)];
      if (chosen != _last) break;
    }
  } else {
    int pos = 0;
    for (int i = 0; i < n; i++) {
      if (cand[i] == _last) {
        pos = i + 1;
        break;
      }
    }
    chosen = cand[pos % n];
  }
  _last = chosen;
  return chosen;
}