// ============================================================
//  uart_link.h - C3 终端 UART 有线通信 (与主控 terminal_link.h 对接)
//
//  通道: C3 UART1 = GPIO18(TX)/GPIO19(RX) (多功能口, 需 USB_HID_ON_BOOT=0 释放)
//  接线:
//    主控 GPIO21 (TX) ──→ C3 GPIO18 (RX)
//    主控 GPIO22 (RX) ←── C3 GPIO19 (TX)
//    GND ──────────────── GND
//
//  协议 (ASCII 文本帧, 与 terminal_link.h 一致):
//    主控→C3:  $S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid,t1_range,t1_angle,t1_velo,t1_id,...\n
//    C3→主控:  $C,key=value\n   /   $C,SAVE\n   /   $C,RESET\n
//
//  延迟 ~10ms (10Hz 推送), 远优于 WiFi 的 200ms, 报警音/显示同步更准
// ============================================================
#pragma once
#include <Arduino.h>

#define TERM_BAUD 115200

// ============ 接收到的状态 (字段与 net_link.h 保持一致, 视图层无需改) ============
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

    TerminalState() { reset(); }
    void reset() {
        obj_num = 0; bz_mode = 0; ind_l = 0; ind_r = 0; turn = 0;
        rcw_l = rcw_r = false; valid = false; rx_bytes = 0; det_range = 25;
        memset(objs, 0, sizeof(objs));
        last_frame_ms = 0; online = false;
    }
};

// 把逗号分隔的 ASCII 帧解析到 TerminalState
// 帧格式: $S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid,t1_range,t1_angle,t1_velo,t1_id,...
// 用 String + indexOf 解析, 避免 strtok 的状态副作用 (C3 单核, 简单可靠)
inline bool parseFrame(const String &frame, TerminalState &out) {
    // frame 已去掉 "$S," 前缀
    // 字段序号:  0=obj_num 1=bz 2=rcw_l 3=rcw_r 4=ind_l 5=ind_r
    //           6=turn   7=rx_bytes 8=valid, 之后每 4 个字段为一个目标
    int idx = 0;
    int start = 0;
    // 先收集所有字段到临时数组
    int fields[40];
    int nfield = 0;
    while (start < (int)frame.length() && nfield < 40) {
        int comma = frame.indexOf(',', start);
        String s = (comma < 0) ? frame.substring(start) : frame.substring(start, comma);
        fields[nfield++] = s.toInt();
        if (comma < 0) break;
        start = comma + 1;
    }
    if (nfield < 9) return false;   // 至少 9 个系统字段

    out.obj_num = fields[0];
    out.bz_mode = fields[1];
    out.rcw_l   = fields[2] != 0;
    out.rcw_r   = fields[3] != 0;
    out.ind_l   = fields[4];
    out.ind_r   = fields[5];
    out.turn    = fields[6];
    out.rx_bytes= fields[7];
    out.valid   = fields[8] != 0;

    // 目标: 从字段 9 开始, 每 4 个一组 (range, angle, velo, id)
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
    String _rx_buf;
    unsigned long _last_rx;

public:
    TerminalState state;

    UartLink() : _serial(nullptr), _last_rx(0) {}

    void init() {
        // C3 UART1: GPIO19=RX, GPIO18=TX (反向, 匹配实测可用的接线)
        Serial1.end();
        Serial1.begin(TERM_BAUD, SERIAL_8N1, 19, 18);  // RX=19, TX=18
        _serial = &Serial1;
        Serial.println("[UART] 终端链路就绪 (115200, RX=19/TX=18)");
    }

    // 主循环调用: 读主控发来的 $S 帧
    void update() {
        if (!_serial) return;
        while (_serial->available()) {
            char c = (char)_serial->read();
            if (c == '\n') {
                _rx_buf.trim();
                if (_rx_buf.startsWith("$S,")) {
                    String body = _rx_buf.substring(3);   // 去掉 "$S,"
                    TerminalState ns = state;              // 保留 det_range 等本地字段
                    if (parseFrame(body, ns)) {
                        ns.last_frame_ms = millis();
                        ns.online = true;
                        state = ns;
                        _last_rx = millis();
                    }
                }
                _rx_buf = "";
            } else if (c != '\r') {
                _rx_buf += c;
                if (_rx_buf.length() > 256) _rx_buf = "";   // 防溢出
            }
        }

        // 3 秒没收到帧 → 离线
        if (state.online && _last_rx > 0 && millis() - _last_rx > 3000) {
            state.online = false;
        }
    }

    bool isOnline() { return state.online; }

    // 触摸提交配置: 发 $C,key=value\n
    // 主控 terminal_link.h 收到后自动 saveToNVS + setBSDMode (与 WiFi POST 等价)
    bool sendConfig(const String &group, const String &key, int value) {
        if (!_serial) return false;
        // 注意: terminal_link.h 按 key 名分发到 config 字段, group 仅用于 C3 侧 UI 分组
        String frame = "$C," + key + "=" + String(value) + "\n";
        _serial->print(frame);
        Serial.printf("[UART] TX %s", frame.c_str());
        return true;
    }

    // 兼容 config_view 旧接口
    void sendSave()  { if (_serial) _serial->print("$C,SAVE\n");  }
    void sendReset() { if (_serial) _serial->print("$C,RESET\n"); }
    bool sendFactoryReset() { sendReset(); return true; }
};

extern UartLink netLink;   // 沿用 netLink 名, c3_terminal.ino 无需改引用
