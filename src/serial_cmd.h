#pragma once
#include <Arduino.h>

// ============================================================
//  后台串口协议 —— 一行一条 JSON 指令，'\n' 结尾
//
//  背景：田野部署后，节点头顶的"后台主机"（树莓派/4G 网关）通过
//  串口和 ESP32 说话。MQTT 管云端策略，串口管"文件级"的重活：
//  日志回传、音频上传/删除/列表、参数配置、状态查询。
//
//  与原有单字符调试命令共用一个 Serial：
//    · 字符以 '{' 开头   -> 进入 JSON 状态机（本模块）
//    · 其它字符          -> 原调试命令（b/t/g/w/...）
//    · 后台解析时只认 '{' 开头的行，人类调试日志直接忽略
//
//  7 条指令（详细格式见 docs/20-后台串口协议.md）：
//    sync_logs     回传 SD 卡事件日志（支持 since 增量 + limit 上限）
//    get_status    电压/存储/温湿度/运行时长等一包状态
//    upload_audio  先回 ready，再收二进制流写入 /sounds，可选 CRC32
//    delete_audio  删除 SD 卡上的音频
//    list_audio    列出音频库
//    set_config    改配置（volume / trigger_interval / ...）-> 存 NVS
//    get_config    读配置
//
//  响应格式：单行 JSON + '\n'，带 "cmd" 回显，后台逐行读。
// ============================================================
class SerialCmd {
public:
  // main.cpp 提供的钩子：本模块不直接依赖播放器/温湿度，解耦用
  struct Hooks {
    void (*activity)()                 = nullptr; // 串口有活动（main 用它推迟深度睡眠）
    bool (*isPlaying)()                = nullptr; // 正在播放？
    void (*stopPlayback)()             = nullptr; // 立刻停止播放
    bool (*readClimate)(float &t, float &h) = nullptr; // 读温湿度，false=传感器不在
  };

  void begin(Hooks hooks);

  // 逐字符喂入。返回 true = 该字符被 JSON 状态机消化了，
  // 调用方（handleSerial）直接处理下一个字符。
  bool charge(char c);

private:
  void handleLine(const String &line);

  void cmdSyncLogs(const String &line);
  void cmdGetStatus();
  void cmdUploadAudio(const String &line);
  bool receiveBinary(const String &path, size_t size,
                     uint32_t crcWant, bool hasCrc, String &err, size_t &got);
  void cmdDeleteAudio(const String &line);
  void cmdListAudio();
  void cmdSetConfig(const String &line);
  void cmdGetConfig();

  Hooks    _hooks;
  bool     _inJson   = false;   // 正在收集一条 JSON 指令
  bool     _overflow = false;   // 行超长，丢弃到换行为止
  String   _line;
  static const size_t MAX_LINE = 512;
};
