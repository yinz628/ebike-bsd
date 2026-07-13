// ============================================================
//  ota_receiver.h - C3 OTA 接收 (经 UART1 接收主控转发)
//
//  从 uart_link.h 拆出, 职责单一: 接收主控转发的 $OTAB/$OTAC/$OTAE 帧,
//  写入 OTA 分区, 回复 ACK/NACK/OK/FAIL.
//
//  封装 C3OtaProgress: 提供 isActive()/getProgress()/timeoutCheck() 访问器,
//  外部 (c3_terminal.ino) 不再直接翻转 c3OtaProgress.active.
//
//  UartLink::update() 把 OTA 帧转发到本类处理.
// ============================================================
#ifndef C3_OTA_RECEIVER_H
#define C3_OTA_RECEIVER_H

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "term_protocol.h"   // OTA_BLOCK_BYTES / termCrc16

// C3 OTA 升级进度 (供屏幕显示 "升级中 NN%")
struct C3OtaProgress {
    bool active = false;
    size_t total = 0;       // 总字节数
    size_t totalSeq = 0;    // 总块数
    size_t curSeq = 0;      // 当前期望块序号
    int percent = 0;
    String version;         // 主控传来的版本号
    unsigned long lastChunkMs = 0;   // 最近一次收到数据块的时间 (超时检测)
};

class OtaReceiver {
private:
    HardwareSerial *_serial;
    bool _ota_active = false;
    size_t _ota_total = 0;
    size_t _ota_total_seq = 0;
    size_t _ota_expect_seq = 0;
    uint8_t _ota_block[OTA_BLOCK_BYTES];
    C3OtaProgress _progress;

    // 发 $C,<body>\n (ACK/NACK/OK/FAIL)
    void sendAck(const char *body) {
        if (!_serial) return;
        String f = "$C,"; f += body; f += "\n";
        _serial->print(f);
    }

    static uint8_t hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

public:
    C3OtaProgress progress;   // 公开进度副本 (供 c3_terminal.ino 读取显示)

    void begin(HardwareSerial *serial) { _serial = serial; }

    // 是否正在进行 OTA (c3_terminal.ino 据此决定画覆盖层还是正常视图)
    bool isActive() const { return _ota_active; }

    const C3OtaProgress& getProgress() const { return progress; }

    // 超时检测: 15 秒没收到新块 → 自动退出 OTA 模式 (封装修正: 外部不再翻 active)
    void timeoutCheck(unsigned long timeoutMs = 15000) {
        if (!_ota_active) return;
        if (progress.lastChunkMs > 0 && millis() - progress.lastChunkMs > timeoutMs) {
            Serial.println(F("[OTA] 超时 (15s 无数据), 自动退出升级模式"));
            _ota_active = false;
            progress.active = false;
        }
    }

    // ---- $OTAB,<size>,<version> ----
    void handleBegin(const char *body) {
        int comma = -1;
        for (int i = 0; body[i]; i++) { if (body[i] == ',') { comma = i; break; } }
        String sizeStr = (comma < 0) ? String(body) : String(body).substring(0, comma);
        size_t size = (size_t)strtoul(sizeStr.c_str(), nullptr, 10);
        if (comma >= 0) progress.version = String(body).substring(comma + 1);

        Serial.printf("[OTA] 收到 $OTAB size=%u\n", (unsigned)size);
        // size 合理性校验: 用运行分区大小做上界, 不硬编码
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        size_t maxAppSize = next ? next->size : (2 * 1024 * 1024);
        if (size == 0 || size > maxAppSize) {
            Serial.printf("[OTA] ERROR: size %u 越界 (max %u)\n", (unsigned)size, (unsigned)maxAppSize);
            sendAck(("OTAFAIL,size_" + String((unsigned)size)).c_str());
            return;
        }
        if (!Update.begin(size)) {
            Serial.println(F("[OTA] ERROR: Update.begin failed"));
            sendAck("OTAFAIL,begin_failed");
            return;
        }
        _ota_active = true;
        _ota_total = size;
        _ota_total_seq = (size + OTA_BLOCK_BYTES - 1) / OTA_BLOCK_BYTES;
        _ota_expect_seq = 0;
        progress.active = true;
        progress.total = size;
        progress.totalSeq = _ota_total_seq;
        progress.curSeq = 0;
        progress.percent = 0;
        progress.lastChunkMs = millis();
        sendAck("OTAR,ready");
    }

    // ---- $OTAC,<seq>,<hex>,<crc> ----
    void handleChunk(const char *body) {
        if (!_ota_active) return;
        int p1 = -1, p2 = -1;
        for (int i = 0; body[i]; i++) {
            if (body[i] == ',') {
                if (p1 < 0) p1 = i;
                else if (p2 < 0) p2 = i;
            }
        }
        if (p1 < 0 || p2 < 0) {
            sendAck(("OTAN," + String(_ota_expect_seq)).c_str());
            return;
        }
        uint32_t seq = (uint32_t)strtoul(String(body).substring(0, p1).c_str(), nullptr, 10);
        String hexPart = String(body).substring(p1 + 1, p2);
        uint16_t crcRecv = (uint16_t)strtoul(String(body).substring(p2 + 1).c_str(), nullptr, 10);

        if (seq != _ota_expect_seq) {
            Serial.printf("[OTA] 乱序块: 收到 %u 期望 %u, NACK\n",
                          (unsigned)seq, (unsigned)_ota_expect_seq);
            sendAck(("OTAR," + String(_ota_expect_seq)).c_str());
            return;
        }

        size_t hexLen = hexPart.length();
        if (hexLen < 2 || hexLen % 2 != 0 || hexLen / 2 > OTA_BLOCK_BYTES) {
            Serial.printf("[OTA] hex 长度异常 (%u), NACK 块 %u\n", (unsigned)hexLen, (unsigned)seq);
            sendAck(("OTAN," + String(seq)).c_str());
            return;
        }
        size_t blen = hexLen / 2;
        for (size_t i = 0; i < blen; i++) {
            _ota_block[i] = (hexVal(hexPart[i * 2]) << 4) | hexVal(hexPart[i * 2 + 1]);
        }

        uint16_t crcCalc = termCrc16(_ota_block, blen);
        if (crcCalc != crcRecv) {
            Serial.printf("[OTA] CRC 错误: 收到 %u 计算 %u, NACK 块 %u\n",
                          crcRecv, crcCalc, (unsigned)seq);
            sendAck(("OTAN," + String(seq)).c_str());
            return;
        }

        size_t w = Update.write(_ota_block, blen);
        if (w != blen) {
            Serial.printf("[OTA] ERROR: write %u != %u\n", (unsigned)w, (unsigned)blen);
            sendAck(("OTAN," + String(seq)).c_str());
            return;
        }

        _ota_expect_seq++;
        progress.curSeq = _ota_expect_seq;
        progress.percent = _ota_total > 0 ? (_ota_expect_seq * 100 / _ota_total_seq) : 0;
        progress.lastChunkMs = millis();
        sendAck(("OTAR," + String(seq)).c_str());
    }

    // ---- $OTAE ----
    void handleEnd() {
        if (!_ota_active) return;
        Serial.printf("[OTA] 收到 $OTAE, 已写 %u/%u 块, 校验并结束\n",
                      (unsigned)_ota_expect_seq, (unsigned)_ota_total_seq);
        if (Update.end(true)) {
            Serial.println(F("[OTA] 校验通过, 升级成功, 即将重启"));
            sendAck("OTAOK");
            progress.percent = 100;
            delay(500);
            ESP.restart();
        } else {
            Serial.print(F("[OTA] ERROR: Update.end 失败: "));
            Update.printError(Serial);
            sendAck(("OTAFAIL," + String(Update.getError())).c_str());
            Update.abort();
            _ota_active = false;
            progress.active = false;
        }
    }
};

#endif // C3_OTA_RECEIVER_H
