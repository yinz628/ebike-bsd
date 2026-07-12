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
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "bsd_protocol.h"
#include "ms60_radar.h"
#include "config_store.h"
#include "term_protocol.h"   // 共享协议: TERM_BAUD (主控↔C3 波特率单一真源)

// ==== 全局变量 extern (定义在 ebike_bsd.ino) ====
extern MS60Radar radar;
extern ConfigStore config;
extern TurnState_t turn_state;
extern int buzzer_mode;
extern bool rcw_l_active, rcw_r_active;
extern int ind_left_mode, ind_right_mode;
extern bool g_wifi_running;             // WiFi AP 开关状态
extern unsigned long g_wifi_idle_since; // AP 空闲计时起点 (0=有连接或未开始计时)
extern AsyncWebServer server;

// ============ 引脚 ============
// UART1: GPIO18(TX)/GPIO19(RX) — 已由 v2.7-c3-display 实测验证可稳定传输
// 历史:
//   - 原 GPIO21/22 (OLED I2C 脚) 输出驱动物理损坏, 方波测试 0V, 弃用
//   - 曾用 GPIO5(TX)/GPIO2(RX): GPIO2 是 strapping 引脚 (启动时电平影响 Flash
//     模式), 做 UART TX 会引入噪声, 实测不稳定
//   - GPIO18/GPIO19 为通用 IO, 无 strapping 约束, v2.7-c3-display 已验证通过
// 接线:
//   主控 TX(GPIO18) ──→ C3 RX(GPIO19)
//   主控 RX(GPIO19) ←── C3 TX(GPIO18)
//   GND ────────────── GND (必须共地)
#define TERM_UART_NUM     1
#define TERM_TX_PIN       18
#define TERM_RX_PIN       19
// TERM_BAUD 来自共享 term_protocol.h (主控↔C3 必须一致)

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

        // ============ OTA 回复 (C3 → 主控) ============
        // 帧格式: $C,OTAR,ready / $C,OTAR,<seq> / $C,OTAN,<seq> / $C,OTAOK / $C,OTAFAIL,<reason>
        // 分发到 ota_manager.h 的转发状态机 (otaRelayCtx).
        if (cmd == "OTAR,ready") { otaRelayOnC3Ready(); return; }
        if (cmd == "OTAOK")      { otaRelayOnOk(); return; }
        if (cmd.startsWith("OTAR,")) {
            // OTAR,<seq> — ACK 当前块
            long seq = cmd.substring(5).toInt();
            otaRelayOnAck((uint32_t)seq);
            return;
        }
        if (cmd.startsWith("OTAN,")) {
            // OTAN,<seq> — NACK, 重传
            long seq = cmd.substring(5).toInt();
            otaRelayOnNack((uint32_t)seq);
            return;
        }
        if (cmd.startsWith("OTAFAIL,")) {
            otaRelayOnFail(cmd.substring(8));
            return;
        }

        // 查询配置: C3 发 $C,GETCFG → 主控回 $CFG,15个值\n (整包回传)
        if (cmd == "GETCFG") {
            char buf[200];
            snprintf(buf, sizeof(buf), "$CFG,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                config.rcw.low_speed,
                config.rcw.speed_threshold,
                config.rcw.range_limit,
                config.rcw.lateral_limit,
                config.rcw.hold_time,
                config.rcw.lflash_interval,
                config.rcw.flash_interval,
                config.turn.speed_threshold,
                config.turn.range_limit,
                config.turn.lateral_limit,
                config.sys.bsd_beep_cooldown,
                config.sys.rcw_buzzer,
                config.radar.det_range,
                config.radar.sensitivity,
                g_wifi_running ? 1 : 0);
            _serial->print(buf);
            Serial.println("[TERM] config sent to C3");
            return;
        }

        // key=value 形式
        int eq = cmd.indexOf('=');
        if (eq < 0) return;
        String key = cmd.substring(0, eq);
        String valStr = cmd.substring(eq + 1);
        int val = valStr.toInt();

        // WiFi 开关: $C,wifi_on=1/0 (C3 触摸面板手动控制)
        // 手动开启后仍由主控 loop 的自动关逻辑接管 (30s 无连接自动关),
        // 不再有"手动开=永久不自动关"的旁路 (历史 g_wifi_manual 已废弃).
        if (key == "wifi_on") {
            if (val) {
                Serial.println("[TERM] WiFi ON (by C3), 30s无连接自动关");
                WiFi.mode(WIFI_AP);
                WiFi.setTxPower(WIFI_TX_POWER);   // 与 initWiFi 一致, 降功率省电
                WiFi.softAP(config.sys.wifi_ssid, config.sys.wifi_pass);
                delay(100);
                server.end(); delay(50); server.begin();
                g_wifi_running = true;
                g_wifi_idle_since = 0;   // 重置空闲计时, 重新开始 30s 倒计时
            } else {
                Serial.println("[TERM] WiFi OFF (by C3)");
                server.end(); delay(50);
                WiFi.softAPdisconnect(true);
                WiFi.enableAP(false);
                WiFi.mode(WIFI_OFF);
                g_wifi_running = false;
            }
            return;
        }

        // 映射到 config 字段 (键名与 wifi_web.h JSON 保持一致)
        bool changed = false;
        if      (key == "rcw_speed")      { config.rcw.speed_threshold = val; changed = true; }
        else if (key == "rcw_low")        { config.rcw.low_speed       = val; changed = true; }
        else if (key == "rcw_range")      { config.rcw.range_limit     = val; changed = true; }
        else if (key == "rcw_lateral")    { config.rcw.lateral_limit   = val; changed = true; }
        else if (key == "rcw_hold")       { config.rcw.hold_time       = val; changed = true; }
        else if (key == "rcw_lflash")     { config.rcw.lflash_interval = val; changed = true; }
        else if (key == "rcw_flash")      { config.rcw.flash_interval  = val; changed = true; }
        else if (key == "rcw_lmin")       { config.rcw.left_angle_min  = val; changed = true; }
        else if (key == "rcw_lmax")       { config.rcw.left_angle_max  = val; changed = true; }
        else if (key == "rcw_rmin")       { config.rcw.right_angle_min = val; changed = true; }
        else if (key == "rcw_rmax")       { config.rcw.right_angle_max = val; changed = true; }
        else if (key == "turn_speed")     { config.turn.speed_threshold= val; changed = true; }
        else if (key == "turn_range")     { config.turn.range_limit    = val; changed = true; }
        else if (key == "turn_lateral")   { config.turn.lateral_limit  = val; changed = true; }
        else if (key == "sensitivity")    { config.radar.sensitivity   = val; changed = true; }
        else if (key == "det_range")      { config.radar.det_range     = val; changed = true; }
        else if (key == "beep_cool")      { config.sys.bsd_beep_cooldown = val; changed = true; }
        else if (key == "rcw_buzzer")     { config.sys.rcw_buzzer = val; changed = true; }

        if (changed) {
            config.sanitize();   // 统一范围校验 (防 C3 传入 0/负数/超限值)
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
        // ESP32 UART1: begin() 已内部完成引脚绑定, 不再额外调 uart_set_pin()
        // (历史教训: 重复调用在某些引脚上会与 begin() 配置冲突, v2.7-c3-display
        //  的成功方案只用 begin(), 故此处对齐)
        _serial->end();
        _serial->begin(TERM_BAUD, SERIAL_8N1, TERM_RX_PIN, TERM_TX_PIN);
        Serial.println("[TERM] UART1 终端链路就绪 (115200, TX=GPIO18/RX=GPIO19)");
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

        // 用栈缓冲替代 String 拼接, 避免 10Hz 高频堆分配/释放导致堆碎片化.
        // 帧格式: $S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid
        //         [,range,angle,velo,id]×N\n
        char frame[200];
        int pos = snprintf(frame, sizeof(frame), "$S,%d,%d,%d,%d,%d,%d,%d,%lu,%d",
            f->obj_num,
            buzzer_mode,
            rcw_l_active ? 1 : 0,
            rcw_r_active ? 1 : 0,
            ind_left_mode,
            ind_right_mode,
            (int)turn_state,
            radar.getTotalBytes(),
            f->valid ? 1 : 0);

        // 各目标: range,angle,velo,id
        int n = min((int)f->obj_num, BSD_MAX_OBJECTS);
        for (int i = 0; i < n && pos < (int)sizeof(frame) - 16; i++) {
            int w = snprintf(frame + pos, sizeof(frame) - pos, ",%d,%d,%d,%d",
                f->objects[i].range,
                f->objects[i].angle,
                f->objects[i].velocity,
                f->objects[i].objId);
            if (w < 0) break;
            pos += w;
        }
        if (pos < (int)sizeof(frame) - 1) {
            frame[pos++] = '\n';
            frame[pos] = '\0';
        }
        _serial->print(frame);
    }
};

// 全局实例
TerminalLink termLink;

inline void terminalLinkInit() { termLink.init(); }
inline void terminalLinkUpdate() { termLink.update(); }

#endif // TERMINAL_LINK_H
