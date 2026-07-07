// ============================================================
//  uart_link.h - C3 终端侧 UART 通信 (与主控 terminal_link.h 对应)
//  接收: $S,... 状态帧  → 更新 TerminalState
//  发送: $C,key=value / $C,SAVE / $C,RESET (触摸触发)
//  UART: GPIO18(RX) / GPIO19(TX), 115200, 复用多功能口
// ============================================================
#pragma once
#include <Arduino.h>

// ============ 引脚 ============
// 实战派 C3 多功能口: GPIO18/19 (默认 USB D-/D+, 复用为 UART)
// 注意: 板载 USB-TTL 芯片走独立 Type-C, 复用此处不影响烧录
#define LINK_RX_PIN   18   // C3 RX ← 主控 TX
#define LINK_TX_PIN   19   // C3 TX → 主控 RX
#define LINK_BAUD     115200

// ============ 接收到的状态 ============
struct TermObj {
    int8_t   range;     // m
    int8_t   angle;     // ° (负=左, 正=右)
    int8_t   velocity;  // m/s
    uint8_t  id;
};

struct TerminalState {
    uint8_t  obj_num;       // 目标数 (0..4)
    uint8_t  bz_mode;       // 蜂鸣模式 0/1/2/3
    uint8_t  ind_l;         // 左指示灯模式 0/1/2/3
    uint8_t  ind_r;         // 右指示灯模式
    uint8_t  turn;          // 转向 0=off 1=left 2=right
    bool     rcw_l, rcw_r;  // 左/右后碰撞预警
    bool     valid;         // 雷达帧有效
    uint32_t rx_bytes;      // 主控雷达累计字节 (诊断)
    TermObj  objs[4];       // 最多 4 个目标

    uint32_t last_frame_ms; // 最近一次收到 $S 的时间
    bool     online;        // 最近 1.5s 内有帧 = true

    TerminalState() { reset(); }
    void reset() {
        obj_num = 0; bz_mode = 0; ind_l = 0; ind_r = 0; turn = 0;
        rcw_l = rcw_r = false; valid = false; rx_bytes = 0;
        memset(objs, 0, sizeof(objs));
        last_frame_ms = 0; online = false;
    }
};

class UartLink {
private:
    HardwareSerial *_serial;
    String _rx_buf;

    // 解析 "$S,..." 状态帧
    void parseStateFrame(const String &line) {
        // line 形如 "$S,1,2,1,0,2,0,1,12345,1,8,-28,4,1"
        // 按 ',' 切分为 parts[]
        String parts[24];
        int p = 0, s = 3;   // 跳过 "$S,"
        while (p < 24) {
            int c = line.indexOf(',', s);
            if (c < 0) { parts[p++] = line.substring(s); break; }
            parts[p++] = line.substring(s, c);
            s = c + 1;
        }

        TerminalState ns;
        int k = 0;
        ns.obj_num  = parts[k++].toInt();
        ns.bz_mode  = parts[k++].toInt();
        ns.rcw_l    = parts[k++].toInt() != 0;
        ns.rcw_r    = parts[k++].toInt() != 0;
        ns.ind_l    = parts[k++].toInt();
        ns.ind_r    = parts[k++].toInt();
        ns.turn     = parts[k++].toInt();
        ns.rx_bytes = (uint32_t)parts[k++].toInt();
        ns.valid    = parts[k++].toInt() != 0;

        int m = min((int)ns.obj_num, 4);
        for (int i = 0; i < m; i++) {
            ns.objs[i].range    = parts[k++].toInt();
            ns.objs[i].angle    = parts[k++].toInt();
            ns.objs[i].velocity = parts[k++].toInt();
            ns.objs[i].id       = parts[k++].toInt();
        }

        // 提交状态
        state = ns;
        state.last_frame_ms = millis();
        state.online = true;

        // 调试输出 (P1 验证用, P2 后可注释)
        static unsigned long last_dbg = 0;
        if (millis() - last_dbg > 1000) {
            last_dbg = millis();
            Serial.printf("[LINK] obj=%d bz=%d turn=%d rcw[%d,%d] valid=%d\n",
                          state.obj_num, state.bz_mode, state.turn,
                          state.rcw_l, state.rcw_r, state.valid);
            for (int i = 0; i < m; i++) {
                Serial.printf("  t%d: %dm %d° %dm/s\n",
                              i, state.objs[i].range, state.objs[i].angle, state.objs[i].velocity);
            }
        }
    }

public:
    TerminalState state;

    UartLink() : _serial(nullptr) {}

    void init() {
        // C3 用 Serial1, 复用 GPIO18/19
        Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
        _serial = &Serial1;
        Serial.println("[LINK] UART 就绪 (RX=18, TX=19, 115200), 等待主控 $S 帧...");
    }

    // 主循环调用: 非阻塞读取
    void update() {
        if (!_serial) return;
        while (_serial->available()) {
            char c = (char)_serial->read();
            if (c == '\n') {
                _rx_buf.trim();
                if (_rx_buf.startsWith("$S,")) {
                    parseStateFrame(_rx_buf);
                }
                _rx_buf = "";
            } else if (c != '\r') {
                _rx_buf += c;
                if (_rx_buf.length() > 128) _rx_buf = "";  // 防溢出
            }
        }

        // 在线检测: 1.5s 无帧 → 离线
        if (state.online && millis() - state.last_frame_ms > 1500) {
            state.online = false;
            state.reset();
        }
    }

    // 触摸事件: 发送配置命令给主控
    void sendConfig(const String &key, int value) {
        String frame = "$C," + key + "=" + String(value) + "\n";
        _serial->print(frame);
        Serial.print("[LINK] 发送: "); Serial.print(frame);
    }

    void sendSave()   { _serial->print("$C,SAVE\n");  Serial.println("[LINK] 发送 SAVE"); }
    void sendReset()  { _serial->print("$C,RESET\n"); Serial.println("[LINK] 发送 RESET"); }
};

extern UartLink link;
