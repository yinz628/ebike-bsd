// ============================================================
//  terminal_link.h - 车尾主控 ↔ 车把 C3 终端 UART 通信
//  通道: UART1 (空闲), 引脚 GPIO27(TX)/GPIO32(RX)
//  协议: ASCII 文本帧, 便于串口监视器调试
//    主控→C3:  $S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid,t1_range,t1_angle,t1_velo,t1_id,...\n
//    C3→主控:  $C,key=value\n   /   $C,SAVE\n   /   $C,RESET\n
// ============================================================
#ifndef TERMINAL_LINK_H
#define TERMINAL_LINK_H

#include <Arduino.h>
#include <driver/uart.h>
#include "bsd_protocol.h"
#include "ms60_radar.h"
#include "config_store.h"

// ==== 全局变量 extern (定义在 ebike_bsd.ino) ====
extern MS60Radar radar;
extern ConfigStore config;
extern TurnState_t turn_state;
extern int buzzer_mode;
extern bool rcw_l_active, rcw_r_active;
extern int ind_left_mode, ind_right_mode;

// ============ 引脚 ============
// UART1: 用 GPIO5(TX)/GPIO2(RX)
// ⚠ 原 GPIO21/22 已废弃: 这两个脚 (原 OLED I2C SDA/SCL) 输出驱动物理损坏,
//    方波测试量得 0V (正常应 ~1.5V), 无法驱动 UART TX 信号.
//    实测主控仅 GPIO2/5/15 输出正常, 故 UART1 改用 GPIO5(TX)/GPIO2(RX).
// 接线:
//   主控 TX(GPIO5)  ──→ C3 RX(GPIO18)
//   主控 RX(GPIO2)  ←── C3 TX(GPIO19)
//   GND ────────────── GND (必须共地)
#define TERM_UART_NUM     1
#define TERM_TX_PIN       5
#define TERM_RX_PIN       2
#define TERM_BAUD         115200

// 推送周期 (与主循环对齐)
#define TERM_PUSH_INTERVAL_MS  100   // 10Hz 推送给 C3 (主循环 50ms, 每 2 次推一次)

class TerminalLink {
private:
    HardwareSerial *_serial;
    unsigned long _last_push;
    String _rx_buf;            // 累积 C3 发来的命令行
    bool _connected;           // 最近 3 秒内收过 C3 数据 = true

    // 解析 "$C,key=value" 并应用到 config
    void applyCommand(const String &cmd) {
        // cmd 形如 "rcw_speed=3" / "SAVE" / "RESET"
        Serial.print("[TERM] C3 cmd: "); Serial.println(cmd);

        if (cmd == "SAVE") {
            config.saveToNVS();
            radar.setBSDMode();
            Serial.println("[TERM] config saved (by C3)");
            return;
        }
        if (cmd == "RESET") {
            config.factoryReset();
            radar.setBSDMode();
            Serial.println("[TERM] factory reset (by C3)");
            return;
        }

        // key=value 形式
        int eq = cmd.indexOf('=');
        if (eq < 0) return;
        String key = cmd.substring(0, eq);
        String valStr = cmd.substring(eq + 1);
        int val = valStr.toInt();

        // 映射到 config 字段 (键名与 wifi_web.h JSON 保持一致)
        bool changed = false;
        if      (key == "rcw_speed")      { config.rcw.speed_threshold = val; changed = true; }
        else if (key == "rcw_low")        { config.rcw.low_speed       = val; changed = true; }
        else if (key == "rcw_range")      { config.rcw.range_limit     = val; changed = true; }
        else if (key == "rcw_hold")       { config.rcw.hold_time       = val; changed = true; }
        else if (key == "rcw_lmin")       { config.rcw.left_angle_min  = val; changed = true; }
        else if (key == "rcw_lmax")       { config.rcw.left_angle_max  = val; changed = true; }
        else if (key == "rcw_rmin")       { config.rcw.right_angle_min = val; changed = true; }
        else if (key == "rcw_rmax")       { config.rcw.right_angle_max = val; changed = true; }
        else if (key == "turn_speed")     { config.turn.speed_threshold= val; changed = true; }
        else if (key == "turn_range")     { config.turn.range_limit    = val; changed = true; }
        else if (key == "sensitivity")    { config.radar.sensitivity   = val; changed = true; }
        else if (key == "det_range")      { config.radar.det_range     = val; changed = true; }
        else if (key == "beep_cool")      { config.sys.bsd_beep_cooldown = val; changed = true; }

        if (changed) {
            // 灵敏度/距离类参数需下发雷达; 其他只存 config
            if (key == "sensitivity" || key == "det_range") {
                radar.setBSDMode();
            }
            Serial.print("[TERM] applied "); Serial.print(key); Serial.print("="); Serial.println(val);
        }
    }

public:
    TerminalLink() : _serial(nullptr), _last_push(0), _connected(false) {}

    void init() {
        _serial = &Serial1;
        // ESP32 UART1: 用 IDF uart_set_pin 强制绑定引脚
        _serial->end();
        _serial->begin(TERM_BAUD, SERIAL_8N1, TERM_RX_PIN, TERM_TX_PIN);
        uart_set_pin(UART_NUM_1, TERM_TX_PIN, TERM_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        Serial.println("[TERM] UART1 终端链路就绪 (115200, TX=GPIO5/RX=GPIO2)");
    }

    // 主循环调用: 推送状态 + 接收命令
    void update() {
        if (!_serial) return;
        unsigned long now = millis();

        // ---- 接收 C3 命令 ----
        while (_serial->available()) {
            char c = (char)_serial->read();
            if (c == '\n') {
                _rx_buf.trim();
                if (_rx_buf.startsWith("$C,")) {
                    applyCommand(_rx_buf.substring(3));   // 去掉 "$C,"
                }
                _rx_buf = "";
                _connected = true;                        // 收到数据 = C3 在线
                _last_rx = now;                           // (复用, 见下)
            } else if (c != '\r') {
                _rx_buf += c;
                if (_rx_buf.length() > 64) _rx_buf = "";  // 防溢出
            }
        }

        // ---- 推送状态帧 (10Hz) ----
        if (now - _last_push >= TERM_PUSH_INTERVAL_MS) {
            _last_push = now;
            pushState();
        }
    }

    unsigned long _last_rx = 0;   // 最近收到 C3 数据的时间 (公开供诊断)
    bool isConnected() {
        // 3 秒内收过 C3 数据视为终端在线
        return _connected && (millis() - _last_rx < 3000);
    }

private:
    // 打包当前状态为 $S 帧并发送
    void pushState() {
        BSDFrame *f = radar.getFrame();

        String frame = "$S,";
        frame += f->obj_num;          frame += ',';
        frame += buzzer_mode;         frame += ',';
        frame += rcw_l_active ? 1:0;  frame += ',';
        frame += rcw_r_active ? 1:0;  frame += ',';
        frame += ind_left_mode;       frame += ',';
        frame += ind_right_mode;      frame += ',';
        frame += (int)turn_state;     frame += ',';
        frame += radar.getTotalBytes(); frame += ',';
        frame += (f->valid ? 1:0);

        // 各目标: range,angle,velo,id (每个目标前加逗号, 避免无目标时尾逗号)
        int n = min((int)f->obj_num, BSD_MAX_OBJECTS);
        for (int i = 0; i < n; i++) {
            frame += ',';
            frame += f->objects[i].range;
            frame += ',';
            frame += f->objects[i].angle;
            frame += ',';
            frame += f->objects[i].velocity;
            frame += ',';
            frame += f->objects[i].objId;
        }
        frame += '\n';

        _serial->print(frame);
    }
};

// 全局实例
TerminalLink termLink;

inline void terminalLinkInit() { termLink.init(); }
inline void terminalLinkUpdate() { termLink.update(); }

#endif // TERMINAL_LINK_H
