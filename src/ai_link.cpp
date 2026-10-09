#include <ArduinoJson.h>
#include "ai_link.h"
#include "config.h"

// —— 动物类别表（与视觉板 ai_infer.cpp 一字不差）——
const char* ANIMAL_EN[AN_COUNT] =
    { "wild_boar", "roe_deer", "squirrel", "badger", "bird", "human" };
const char* ANIMAL_CN[AN_COUNT] =
    { "野猪", "狍子", "松鼠", "獾子", "飞鸟", "人类" };

// 专属音频：放 TF 卡 /sounds/ 下，名字必须一致。
// 缺哪个文件就自动回退 v1 的通用曲库（policy.sounds），不影响触发响应。
const char* ANIMAL_WAV[AN_COUNT] =
    { "wild_boar.wav", "roe_deer.wav", "squirrel.wav", "badger.wav", "bird.wav", "human.wav" };

// 驱赶灯色：野猪红 / 狍子橙 / 松鼠黄 / 獾子紫 / 飞鸟青 / 人类蓝
const uint8_t ANIMAL_RGB[AN_COUNT][3] = {
    {255,   0,   0},    // wild_boar
    {255,  70,   0},    // roe_deer
    {255, 190,   0},    // squirrel
    {170,   0, 255},    // badger
    {  0, 220, 255},    // bird
    {  0,  90, 255},    // human
};

void AiLink::begin()
{
    _uart.setRxBufferSize(1024);
    _uart.begin(AI_UART_BAUD, SERIAL_8N1, PIN_AI_RX, PIN_AI_TX);
}

void AiLink::sendLine(const char *s)
{
    _uart.print(s);
    _uart.print('\n');
    _sent++;
    Serial.printf("[AI->] %s\n", s);
}

void AiLink::ping() { sendLine("{\"cmd\":\"ping\"}"); }

void AiLink::requestCapture(const char *zoneName)
{
    sendLine("{\"cmd\":\"capture\"}");
    _awaiting = true;
    _deadline = millis() + AI_RESULT_TIMEOUT_MS;
    Serial.printf("[AI] 已请求抓拍（分区=%s），%lums 内等结果\n",
                  zoneName, (unsigned long)AI_RESULT_TIMEOUT_MS);
}

static int findClass(const char *en)
{
    for (uint8_t i = 0; i < AN_COUNT; i++)
        if (strcmp(ANIMAL_EN[i], en) == 0) return (int)i;
    return -1;
}

bool AiLink::handleLine(const char *line, AiHit &out)
{
    if (!line[0]) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        Serial.printf("[AI<-] 非JSON(%s): %s\n", err.c_str(), line);
        return false;
    }

    const char *type = doc["type"] | "";
    if (strcmp(type, "result") == 0) {
        const char *en = doc["animal"] | "";

        // 只要回的是 result，视觉板就是有响应的：等待标志先收掉，
        // 免得 unknown（不驱赶的目标）回来后还傻等到超时
        bool wasAwaiting = _awaiting;
        if (wasAwaiting) {
            _awaiting = false;
            _lastResultMs = millis();
            _recv++;
        }

        int cls = findClass(en);
        if (cls < 0) {
            // unknown / 未收录动物：按约定不驱赶（人类、宠物等都在这里）
            if (wasAwaiting)
                Serial.printf("[AI<-] 非目标(%s)：按约定不驱赶，仅留串口记录\n", en);
            else
                Serial.printf("[AI<-] 非请求期的主动上报(%s)，忽略\n", en);
            return false;
        }
        if (!wasAwaiting) {
            // 视觉板自检模式（AUTO_CAPTURE_SEC>0）的主动上报：记数但不动作，
            // 否则田野里没人触发它也会 10 秒吼一次
            Serial.println("[AI<-] 非请求期的主动上报，忽略（不播放/不爆闪）");
            return false;
        }
        out.cls   = (uint8_t)cls;
        out.en    = ANIMAL_EN[cls];
        out.cn    = ANIMAL_CN[cls];
        out.audio = ANIMAL_WAV[cls];
        out.r = ANIMAL_RGB[cls][0];
        out.g = ANIMAL_RGB[cls][1];
        out.b = ANIMAL_RGB[cls][2];
        out.conf = doc["confidence"] | 0.0f;
        if (out.conf < 0.0f) out.conf = 0.0f;
        if (out.conf > 1.0f) out.conf = 1.0f;
        return true;
    }
    if (strcmp(type, "pong") == 0) {
        Serial.println("[AI<-] pong —— 视觉板链路正常");
        _recv++;
        _lastResultMs = millis();
        return false;
    }
    Serial.printf("[AI<-] 未知类型: %s\n", line);
    return false;
}

bool AiLink::poll(AiHit &out)
{
    // 请求超时：视觉板没回（不在线/接线反了），不影响主流程
    if (_awaiting && (int32_t)(millis() - _deadline) >= 0) {
        _awaiting = false;
        Serial.println("[AI] 等结果超时 —— 查视觉板供电 / TX-RX 是否交叉接对");
    }

    bool got = false;
    while (_uart.available()) {
        char ch = (char)_uart.read();
        if (ch == '\n') {
            _line[_len] = '\0';
            if (handleLine(_line, out)) got = true;
            _len = 0;
        } else if (ch != '\r') {
            if (_len < sizeof(_line) - 1) {
                _line[_len++] = ch;
            } else {
                _len = 0;
                Serial.println("[AI<-] 行超长丢弃");
            }
        }
    }
    return got;
}

const char *AiLink::statusText() const
{
    if (_awaiting) return "等待识别结果";
    if (_lastResultMs) return "链路正常(收到过消息)";
    return "无消息(未联调)";
}
