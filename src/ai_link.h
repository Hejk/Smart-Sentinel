#pragma once
#include <Arduino.h>
#include "config.h"

// 6 类识别目标 —— 与视觉板 ai_infer.h 保持同一张表（顺序不能乱）
enum AnimalClass : uint8_t {
    AN_WILD_BOAR = 0,   // 野猪
    AN_ROE_DEER,        // 狍子
    AN_SQUIRREL,        // 松鼠
    AN_BADGER,          // 獾子
    AN_BIRD,            // 飞鸟
    AN_HUMAN,           // 人类
    AN_COUNT
};

extern const char*   ANIMAL_EN[AN_COUNT];
extern const char*   ANIMAL_CN[AN_COUNT];
extern const char*   ANIMAL_WAV[AN_COUNT];     // /sounds/ 里的专属音频文件名
extern const uint8_t ANIMAL_RGB[AN_COUNT][3];  // 识别到后的驱赶灯色

// 一次识别结果（poll 返回 true 时有效）
struct AiHit {
    uint8_t    cls;       // AnimalClass
    const char *en;       // "wild_boar"
    const char *cn;       // "野猪"
    const char *audio;    // "wild_boar.wav"
    uint8_t    r, g, b;   // 对应驱赶灯色
    float      conf;      // 0~1
};

// ============================================================
//  V2 · AI 视觉板 UART1 链路（主控侧）
//
//  协议（一行一条，'\n' 结尾，115200 8N1）：
//    主控→视觉  {"cmd":"capture"}
//    视觉→主控  {"type":"result","animal":"wild_boar","confidence":0.85,"label_cn":"野猪"}
//    链路自检   {"cmd":"ping"} <-> {"type":"pong","from":"ai"}
//
//  接线（交叉）：本板 TX17 → 视觉板 RX18；视觉板 TX17 → 本板 RX18
// ============================================================
class AiLink {
public:
    void begin();
    void ping();                                 // 链路自检
    void requestCapture(const char *zoneName);   // 触发时喊视觉板抓拍
    // 主循环推动；返回 true = 收到一条"请求期的"识别结果（out 已填好）
    bool poll(AiHit &out);
    bool awaiting() const { return _awaiting; }  // 已请求还没回结果（用于推迟睡眠）
    const char *statusText() const;
    uint32_t sentCount() const { return _sent; }
    uint32_t recvCount() const { return _recv; }

private:
    bool handleLine(const char *line, AiHit &out);
    void sendLine(const char *s);

    HardwareSerial _uart{1};
    char     _line[192];
    size_t   _len = 0;
    bool     _awaiting = false;
    uint32_t _deadline = 0;
    uint32_t _lastResultMs = 0;
    uint32_t _sent = 0;
    uint32_t _recv = 0;
};
