// ============================================================
//  uart_link.h - C3 终端 UART 有线通信 (与主控 terminal_link.h 对接)
//
//  通道: C3 UART1 = GPIO18(TX)/GPIO19(RX) (多功能口, 需 USB_HID_ON_BOOT=0 释放)
//  接线 (与主控 terminal_link.h 的 GPIO18/19 交叉对接):
//    主控 TX(GPIO18) ──→ C3 RX(GPIO19)
//    主控 RX(GPIO19) ←── C3 TX(GPIO18)
//    GND ────────────── GND
//
//  职责 (已拆分, 见 ota_receiver.h / ota_guard.h):
//    本文件: $S 状态帧解析 + $CFG 配置帧解析 + 触摸命令发送 + OTA 帧转发
//    ota_receiver.h: OTA 接收 (handleBegin/Chunk/End + CRC + 进度封装)
//    ota_guard.h: 回滚保护 (boot guard + verifyRollbackLater 覆盖)
//
//  延迟 ~10ms (10Hz 推送), 远优于 WiFi 的 200ms
// ============================================================
#pragma once
#include <Arduino.h>
#include "term_protocol.h"   // TERM_BAUD
#include "ota_receiver.h"    // OtaReceiver (OTA 帧转发目标)

// ============ 接收到的状态 (字段与主控 terminal_link.h 保持一致) ============
struct TermObj {
    int8_t  range;
    int8_t  angle;
    int8_t  velocity;
    uint8_t id;
};

struct TerminalState {
    uint8_t  obj_num;
    uint8_t  bz_mode;
    uint8_t  ind_l;
    uint8_t  ind_r;
    uint8_t  turn;
    bool     rcw_l, rcw_r;
    bool     valid;
    uint32_t rx_bytes;
    int      det_range;
    TermObj  objs[4];

    uint32_t last_frame_ms;
    bool     online;

    // 主控回传的配置 (由 $CFG 帧填充, config_view 读取)
    // 顺序与主控 terminal_link.h GETCFG 响应一致 (15 个值):
    //   0:rcw_low 1:rcw_speed 2:rcw_range 3:rcw_lateral 4:rcw_hold 5:rcw_lflash 6:rcw_flash
    //   7:turn_speed 8:turn_range 9:turn_lateral 10:beep_cool 11:rcw_buzzer 12:det_range 13:sensitivity 14:wifi_on
    int cfg[15];
    bool cfg_valid;
    uint8_t cfg_seq;
    bool wifi_on;

    TerminalState() { reset(); }
    void reset() {
        obj_num = 0; bz_mode = 0; ind_l = 0; ind_r = 0; turn = 0;
        rcw_l = rcw_r = false; valid = false; rx_bytes = 0; det_range = 25;
        memset(objs, 0, sizeof(objs));
        memset(cfg, 0, sizeof(cfg));
        cfg_valid = false; cfg_seq = 0; wifi_on = false;
        last_frame_ms = 0; online = false;
    }
    // 清除来自主控的动态状态 (目标/报警/指示), 保留本地配置 (cfg/det_range).
    void clearDynamic() {
        obj_num = 0; bz_mode = 0; ind_l = 0; ind_r = 0; turn = 0;
        rcw_l = rcw_r = false; valid = false;
        memset(objs, 0, sizeof(objs));
    }
};

// 把逗号分隔的 ASCII 帧解析到 TerminalState
inline bool parseFrame(const String &frame, TerminalState &out) {
    int idx = 0;
    int start = 0;
    int fields[40];
    int nfield = 0;
    while (start < (int)frame.length() && nfield < 40) {
        int comma = frame.indexOf(',', start);
        String s = (comma < 0) ? frame.substring(start) : frame.substring(start, comma);
        fields[nfield++] = s.toInt();
        if (comma < 0) break;
        start = comma + 1;
    }
    if (nfield < 9) return false;

    out.obj_num = fields[0];
    out.bz_mode = fields[1];
    out.rcw_l   = fields[2] != 0;
    out.rcw_r   = fields[3] != 0;
    out.ind_l   = fields[4];
    out.ind_r   = fields[5];
    out.turn    = fields[6];
    out.rx_bytes= fields[7];
    out.valid   = fields[8] != 0;

    int m = min((int)out.obj_num, 4);
    for (int i = 0; i < m; i++) {
        int base = 9 + i * 4;
        if (base + 3 >= nfield) break;
        out.objs[i].range    = (int8_t)fields[base + 0];
        out.objs[i].angle    = (int8_t)fields[base + 1];
        out.objs[i].velocity = (int8_t)fields[base + 2];
        out.objs[i].id       = (uint8_t)fields[base + 3];
    }
    return true;
}

class UartLink {
private:
    HardwareSerial *_serial;
    // 接收缓冲: 容纳 $OTAC 帧 (帧头+128B hex=256字符+帧尾 ≈ 285), 故放大到 320.
    char _rx_buf[320];
    int  _rx_len;
    unsigned long _last_rx;

public:
    TerminalState state;
    OtaReceiver otaReceiver;   // OTA 接收器 (update 转发 OTA 帧给它)

    UartLink() : _serial(nullptr), _rx_len(0), _last_rx(0) {
        _rx_buf[0] = '\0';
    }

    void init() {
        // C3 UART1: GPIO19=RX, GPIO18=TX (与主控 GPIO18/19 交叉对接)
        Serial1.end();
        Serial1.begin(TERM_BAUD, SERIAL_8N1, 19, 18);  // RX=19, TX=18
        _serial = &Serial1;
        otaReceiver.begin(_serial);   // OTA 接收器共用同一串口
        Serial.println("[UART] 终端链路就绪 (115200, RX=19/TX=18)");
    }

    // 主循环调用: 读 UART, 分发 $S/$CFG/$OTA* 帧
    void update() {
        if (!_serial) return;
        while (_serial->available()) {
            char c = (char)_serial->read();
            if (c == '\n') {
                _rx_buf[_rx_len] = '\0';
                char *p = _rx_buf;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "$S,", 3) == 0) {
                    String body(p + 3);
                    TerminalState ns = state;
                    if (parseFrame(body, ns)) {
                        ns.last_frame_ms = millis();
                        ns.online = true;
                        state = ns;
                        _last_rx = millis();
                    }
                }
                else if (strncmp(p, "$CFG,", 5) == 0) {
                    parseCfg(String(p + 5));
                }
                // OTA 帧转发给 OtaReceiver (通信与 OTA 解耦)
                else if (strncmp(p, "$OTAB,", 6) == 0) otaReceiver.handleBegin(p + 6);
                else if (strncmp(p, "$OTAC,", 6) == 0) otaReceiver.handleChunk(p + 6);
                else if (strcmp(p, "$OTAE") == 0)      otaReceiver.handleEnd();
                _rx_len = 0;
                _rx_buf[0] = '\0';
            } else if (c != '\r') {
                if (_rx_len < (int)sizeof(_rx_buf) - 1) {
                    _rx_buf[_rx_len++] = c;
                } else {
                    _rx_len = 0;
                    _rx_buf[0] = '\0';
                }
            }
        }

        // 3 秒没收到帧 → 离线, 清除动态状态
        if (state.online && _last_rx > 0 && millis() - _last_rx > 3000) {
            state.online = false;
            state.clearDynamic();
        }
    }

    bool isOnline() { return state.online; }

    // 触摸提交配置: 发 $C,key=value\n
    bool sendConfig(const String &group, const String &key, int value) {
        if (!_serial) return false;
        String frame = "$C," + key + "=" + String(value) + "\n";
        _serial->print(frame);
        Serial.printf("[UART] TX %s", frame.c_str());
        return true;
    }

    // 触摸提交配置保存
    void sendSave()  { if (_serial) _serial->print("$C,SAVE\n"); }

    // 查询主控当前配置: 发 $C,GETCFG → 主控回 $CFG,15个值
    void requestConfig() {
        if (_serial) {
            _serial->print("$C,GETCFG\n");
            Serial.println("[UART] TX $C,GETCFG");
        }
    }

    // WiFi 开关: 发 $C,wifi_on=1/0
    void sendWifi(bool on) {
        if (_serial) {
            String f = "$C,wifi_on=" + String(on ? 1 : 0) + "\n";
            _serial->print(f);
            Serial.printf("[UART] TX %s", f.c_str());
        }
    }

private:
    // 解析 $CFG,v0,...,v14 → state.cfg[] + state.wifi_on (15 个值)
    void parseCfg(const String &body) {
        int start = 0;
        int vals[15]; int n = 0;
        while (start < (int)body.length() && n < 15) {
            int comma = body.indexOf(',', start);
            String s = (comma < 0) ? body.substring(start) : body.substring(start, comma);
            vals[n++] = s.toInt();
            if (comma < 0) break;
            start = comma + 1;
        }
        if (n >= 9) {
            for (int i = 0; i < n && i < 15; i++) state.cfg[i] = vals[i];
            state.wifi_on = (n >= 15) ? (vals[14] != 0) : false;
            state.cfg_valid = true;
            state.cfg_seq++;
            Serial.printf("[UART] got $CFG (%d vals, wifi=%d)\n", n, state.wifi_on);
        }
    }
};

extern UartLink netLink;        // 定义在 c3_terminal.ino
