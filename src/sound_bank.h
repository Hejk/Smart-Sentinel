#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SPI.h>

// ============================================================
//  本地音频库
//  优先用 TF 卡（容量大、拔卡就能换音频），挂不上就退回板载 Flash
//  两个后端的目录结构一样：<根目录>/sounds/*.wav
//    TF 卡    -> /sd/sounds/
//    板载Flash -> /sounds/
//
//  加音频：把 16kHz 单声道 16bit 的 wav 放进卡里的 /sounds 目录
// ============================================================
class SoundBank {
public:
  bool begin();
  bool mounted() const { return _fs != nullptr; }
  bool sdMounted() const { return _sdOk; }   // TF 卡当前是否挂载成功
  bool retrySD();                            // 没挂上时再试一次（成功则切到 TF 卡）
  const char *backendName() const { return _backend; }
  const char *rootPath() const { return _root.c_str(); }

  int count() const { return _count; }
  const String &name(int i) const { return _names[i]; }

  // —— 后台串口协议（serial_cmd）用的扩展口 ——
  fs::FS *filesystem() { return _fs; }     // 当前存储后端（TF 卡或 LittleFS），未挂载为 nullptr
  void    refresh()    { scan(); }         // 上传/删除音频后重建曲库列表

  int  pick(const String &preferList, bool randomPick);
  int  find(const String &name) const { return indexOf(name); }
  File open(int idx);
  void printAll() const;

private:
  bool mountSD();
  bool mountFS();
  void scan();
  void addFile(const String &name);
  int  indexOf(const String &name) const;
  void sdProbe();                   // SPI 裸探测：诊断卡到底有没有应答
  bool sdWarmup(bool verbose);      // 400kHz 手动全握手：CMD0/8/ACMD41/58 追踪+预初始化
  bool finishMount();               // 挂载成功后的卡片信息打印
  bool _initTraced = false;         // 本次开机是否已打印过全握手追踪

  // ★ 挂载与裸探测共用的专用 SPI 实例。注意 core 2.0.x 的 SD 库会
  //   忽略自定义引脚、一律用板子默认脚位，所以 config.h 里的引脚必须
  //   与 S3 默认 SPI 脚（SCK=12 MISO=13 MOSI=11 CS=10）保持一致。
  SPIClass _sdSPI{FSPI};

  static const int MAX_SOUNDS = 32;
  String _names[MAX_SOUNDS];
  int    _count   = 0;
  int    _last    = -1;
  bool   _sdOk    = false;
  fs::FS *_fs     = nullptr;
  String _root    = "/sounds";
  const char *_backend = "未挂载";
};