// ============================================================
//  uart_link.h - C3 终端 UART 有线通信 (与主控 terminal_link.h 对接)
//
//  通道: C3 UART1 = GPIO18(TX)/GPIO19(RX) (多功能口, 需 USB_HID_ON_BOOT=0 释放)
//  接线 (与主控 terminal_link.h 的 GPIO18/19 交叉对接):
//    主控 TX(GPIO18) ──→ C3 RX(GPIO19)
//    主控 RX(GPIO19) ←── C3 TX(GPIO18)
//    GND ────────────── GND
//  (历史: 曾用主控 GPIO5/2, GPIO2 是 strapping 脚, UART 不稳; 改 18/19 已验证)
//
//  协议 (ASCII 文本帧, 与 terminal_link.h 一致):
//    主控→C3:  $S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid,t1_range,t1_angle,t1_velo,t1_id,...\n
//    C3→主控:  $C,key=value\n   /   $C,SAVE\n   /   $C,RESET\n
//
//  延迟 ~10ms (10Hz 推送), 远优于 WiFi 的 200ms, 报警音/显示同步更准
// ============================================================
#pragma once
#include <Arduino.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "term_protocol.h"   // 共享协议: TERM_BAUD / OTA_BLOCK_BYTES / termCrc16

// TERM_BAUD 和 OTA_BLOCK_BYTES 来自共享 term_protocol.h (与主控单一真源)

// ============ OTA 升级 (经 UART1 接收主控转发) ============
// 协议帧 (与 firmware/src/ota_manager.h 对应):
//   主控→C3:  $OTAB,<size>,<version>           (开始)
//             $OTAC,<seq>,<hex(2*128)>,<crc16>  (分块)
//             $OTAE                             (结束)
//   C3→主控:  $C,OTAR,ready / $C,OTAR,<seq>     (ACK)
//             $C,OTAN,<seq>                      (NACK, 重传)
//             $C,OTAOK / $C,OTAFAIL,<reason>     (结束)
// OTA_BLOCK_BYTES 来自共享 term_protocol.h

// C3 OTA 升级进度 (供屏幕显示 "升级中 NN%")
struct C3OtaProgress {
    bool active = false;
    size_t total = 0;       // 总字节数
    size_t totalSeq = 0;    // 总块数
    size_t curSeq = 0;      // 当前期望块序号
    int percent = 0;
    String version;         // 主控传来的版本号
    unsigned long lastChunkMs = 0;   // 最近一次收到数据块的时间 (用于超时检测)
};
extern C3OtaProgress c3OtaProgress;

// CRC16 来自共享 term_protocol.h (termCrc16), 不再本地重复定义

// 是否刚从 OTA 升级启动 (本槽待确认), setup() 末尾调 mark_valid
inline bool c3OtaIsPendingVerify() {
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

inline void c3OtaMarkValid() {
    if (c3OtaIsPendingVerify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println(F("[OTA] 本槽已标记 valid (取消回滚挂起)"));
        // 清 boot guard 失败计数
        Preferences prefs;
        if (prefs.begin("ota_guard", false)) {
            prefs.putInt("bootfails", 0);
            prefs.end();
        }
    }
}

// ============================================================
//  应用层主动回滚保护 (补足 bootloader 默认不自动回滚的缺口)
//
//  Arduino-ESP32 core 的 initArduino() (app_main, 早于 setup) 默认会自动调
//  esp_ota_mark_app_valid_cancel_rollback() 把新槽标记 VALID, 绕过 pending_verify
//  → 让 bootloader 回滚失效. 实测确认 (主控端同一问题已验证).
//
//  覆盖 weak 函数 verifyRollbackLater()=true 阻止 core 自动确认,
//  让新槽保持 pending, 由下面的 boot guard 统一处理.
// ============================================================
extern "C" {
    bool verifyRollbackLater() { return true; }
}

#define C3_OTA_BOOTGUARD_MAX 3
inline void c3OtaBootGuardBegin() {
    if (!c3OtaIsPendingVerify()) return;   // 已确认好槽, 不计数

    Preferences prefs;
    if (!prefs.begin("ota_guard", false)) return;
    int fails = prefs.getInt("bootfails", 0) + 1;
    prefs.putInt("bootfails", fails);
    prefs.end();

    Serial.printf("[OTA-GUARD] 新固件第 %d/%d 次尝试启动\n", fails, C3_OTA_BOOTGUARD_MAX);
    if (fails >= C3_OTA_BOOTGUARD_MAX) {
        Serial.println(F("[OTA-GUARD] 连续启动失败超阈值 → 强制回滚"));
        if (prefs.begin("ota_guard", false)) {
            prefs.putInt("bootfails", 0);
            prefs.end();
        }
        Serial.flush();
        delay(100);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

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

    // 主控回传的配置 (由 $CFG 帧填充, config_view 读取)
    // 顺序与主控 terminal_link.h GETCFG 响应一致 (15 个值):
    //   0:rcw_low 1:rcw_speed 2:rcw_range 3:rcw_lateral 4:rcw_hold 5:rcw_lflash 6:rcw_flash
    //   7:turn_speed 8:turn_range 9:turn_lateral 10:beep_cool 11:rcw_buzzer 12:det_range 13:sensitivity 14:wifi_on
    int cfg[15];
    bool cfg_valid;       // 是否已收到过主控配置
    uint8_t cfg_seq;      // $CFG 帧接收计数 (每次收到+1, config_view 据此判断是否需同步)
    bool wifi_on;         // WiFi 开关状态 (cfg[14] 的镜像, 便于 config_view 直接读)

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
    // 主控离线时调用, 防止幽灵目标和残留报警. 新增动态字段时在此集中维护.
    void clearDynamic() {
        obj_num = 0; bz_mode = 0; ind_l = 0; ind_r = 0; turn = 0;
        rcw_l = rcw_r = false; valid = false;
        memset(objs, 0, sizeof(objs));
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
    // 接收缓冲: 容纳 $OTAC 帧 (帧头+128B hex=256字符+帧尾 ≈ 285), 故放大到 320.
    // 其余 $S/$CFG 帧远小于此.
    char _rx_buf[320];
    int  _rx_len;               // 缓冲当前长度
    unsigned long _last_rx;

    // ---- OTA 接收状态 ----
    bool _ota_active = false;
    size_t _ota_total = 0;
    size_t _ota_total_seq = 0;
    size_t _ota_expect_seq = 0;   // 期望收到的下一块序号
    uint8_t _ota_block[OTA_BLOCK_BYTES];

public:
    TerminalState state;

    UartLink() : _serial(nullptr), _rx_len(0), _last_rx(0) {
        _rx_buf[0] = '\0';
    }

    void init() {
        // C3 UART1: GPIO19=RX, GPIO18=TX (与主控 GPIO18/19 交叉对接)
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
                _rx_buf[_rx_len] = '\0';   // 结束字符串
                // 跳过前导空白
                char *p = _rx_buf;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "$S,", 3) == 0) {
                    String body(p + 3);             // 仅在完整帧时创建一次 String (去掉 "$S,")
                    TerminalState ns = state;       // 保留 det_range 等本地字段
                    if (parseFrame(body, ns)) {
                        ns.last_frame_ms = millis();
                        ns.online = true;
                        state = ns;
                        _last_rx = millis();
                    }
                }
                else if (strncmp(p, "$CFG,", 5) == 0) {
                    // 主控回传配置: $CFG,v0,v1,...,v14 (15 个值, 最后一个是 wifi_on)
                    parseCfg(String(p + 5));
                }
                else if (strncmp(p, "$OTAB,", 6) == 0) handleOtaBegin(p + 6);
                else if (strncmp(p, "$OTAC,", 6) == 0) handleOtaChunk(p + 6);
                else if (strcmp(p, "$OTAE") == 0)      handleOtaEnd();
                _rx_len = 0;       // 重置缓冲
                _rx_buf[0] = '\0';
            } else if (c != '\r') {
                if (_rx_len < (int)sizeof(_rx_buf) - 1) {
                    _rx_buf[_rx_len++] = c;   // 无堆分配的字符累积
                } else {
                    _rx_len = 0;              // 溢出保护: 丢弃重置
                    _rx_buf[0] = '\0';
                }
            }
        }

        // 3 秒没收到帧 → 离线, 清除动态状态 (防幽灵目标 + 残留报警, 安全要求)
        if (state.online && _last_rx > 0 && millis() - _last_rx > 3000) {
            state.online = false;
            state.clearDynamic();
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
            state.cfg_seq++;   // 通知 config_view 有新配置到达
            Serial.printf("[UART] got $CFG (%d vals, wifi=%d)\n", n, state.wifi_on);
        }
    }

    // ============ OTA 接收处理 ============
    void otaSendAck(const char *body) {
        String f = "$C,"; f += body; f += "\n";
        _serial->print(f);
    }

    // hex 字符 → 数值
    static uint8_t hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    // $OTAB,<size>,<version>
    void handleOtaBegin(const char *body) {
        // body 形如 "393216,V0.2"
        int comma = -1;
        for (int i = 0; body[i]; i++) { if (body[i] == ',') { comma = i; break; } }
        String sizeStr = (comma < 0) ? String(body) : String(body).substring(0, comma);
        size_t size = (size_t)strtoul(sizeStr.c_str(), nullptr, 10);
        if (comma >= 0) c3OtaProgress.version = String(body).substring(comma + 1);

        Serial.printf("[OTA] 收到 $OTAB size=%u\n", (unsigned)size);
        // size 合理性校验: 必须大于 0 且不超过当前 OTA 槽可用空间.
        // 用 esp_ota_get_next_update_partition 动态获取目标槽大小, 不硬编码.
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        size_t maxAppSize = next ? next->size : (2 * 1024 * 1024);   // 兜底 2MB
        if (size == 0 || size > maxAppSize) {
            Serial.printf("[OTA] ERROR: size %u 越界 (max %u)\n", (unsigned)size, (unsigned)maxAppSize);
            otaSendAck(("OTAFAIL,size_" + String((unsigned)size)).c_str());
            return;
        }
        if (!Update.begin(size)) {
            Serial.println(F("[OTA] ERROR: Update.begin failed"));
            otaSendAck("OTAFAIL,begin_failed");
            return;
        }
        _ota_active = true;
        _ota_total = size;
        _ota_total_seq = (size + OTA_BLOCK_BYTES - 1) / OTA_BLOCK_BYTES;
        _ota_expect_seq = 0;
        c3OtaProgress.active = true;
        c3OtaProgress.total = size;
        c3OtaProgress.totalSeq = _ota_total_seq;
        c3OtaProgress.curSeq = 0;
        c3OtaProgress.percent = 0;
        c3OtaProgress.lastChunkMs = millis();
        otaSendAck("OTAR,ready");
    }

    // $OTAC,<seq>,<hex>,<crc16>
    void handleOtaChunk(const char *body) {
        if (!_ota_active) return;
        // 帧格式: <seq>,<hex>,<crc>   (2 个逗号, 3 个字段)
        // body 由 update() 去掉了 "$OTAC," 前缀和行尾, crc 在末尾到字符串结束
        int p1 = -1, p2 = -1;
        for (int i = 0; body[i]; i++) {
            if (body[i] == ',') {
                if (p1 < 0) p1 = i;
                else if (p2 < 0) p2 = i;
            }
        }
        if (p1 < 0 || p2 < 0) {
            otaSendAck(("OTAN," + String(_ota_expect_seq)).c_str());
            return;
        }
        uint32_t seq = (uint32_t)strtoul(String(body).substring(0, p1).c_str(), nullptr, 10);
        String hexPart = String(body).substring(p1 + 1, p2);
        uint16_t crcRecv = (uint16_t)strtoul(String(body).substring(p2 + 1).c_str(), nullptr, 10);

        // 期望序号校验 (乱序/重传 → NACK 要求重发期望块)
        if (seq != _ota_expect_seq) {
            Serial.printf("[OTA] 乱序块: 收到 %u 期望 %u, NACK\n",
                          (unsigned)seq, (unsigned)_ota_expect_seq);
            otaSendAck(("OTAR," + String(_ota_expect_seq)).c_str());
            return;
        }

        // hex 解码: 必须是非空偶数长度, 且解码后不超过单块上限
        // (空块 hexLen=0 会让 Update.write(_,0) 行为未定义, 必须拒绝)
        size_t hexLen = hexPart.length();
        if (hexLen < 2 || hexLen % 2 != 0 || hexLen / 2 > OTA_BLOCK_BYTES) {
            Serial.printf("[OTA] hex 长度异常 (%u), NACK 块 %u\n", (unsigned)hexLen, (unsigned)seq);
            otaSendAck(("OTAN," + String(seq)).c_str());
            return;
        }
        size_t blen = hexLen / 2;
        for (size_t i = 0; i < blen; i++) {
            _ota_block[i] = (hexVal(hexPart[i * 2]) << 4) | hexVal(hexPart[i * 2 + 1]);
        }

        // CRC 校验
        uint16_t crcCalc = termCrc16(_ota_block, blen);
        if (crcCalc != crcRecv) {
            Serial.printf("[OTA] CRC 错误: 收到 %u 计算 %u, NACK 块 %u\n",
                          crcRecv, crcCalc, (unsigned)seq);
            otaSendAck(("OTAN," + String(seq)).c_str());
            return;
        }

        // 写入
        size_t w = Update.write(_ota_block, blen);
        if (w != blen) {
            Serial.printf("[OTA] ERROR: write %u != %u\n", (unsigned)w, (unsigned)blen);
            otaSendAck(("OTAN," + String(seq)).c_str());
            return;
        }

        _ota_expect_seq++;
        c3OtaProgress.curSeq = _ota_expect_seq;
        c3OtaProgress.percent = _ota_total > 0 ? (_ota_expect_seq * 100 / _ota_total_seq) : 0;
        c3OtaProgress.lastChunkMs = millis();   // 更新最后接收时间
        // ACK 当前块 (主控据此推进)
        otaSendAck(("OTAR," + String(seq)).c_str());
    }

    // $OTAE
    void handleOtaEnd() {
        if (!_ota_active) return;
        Serial.printf("[OTA] 收到 $OTAE, 已写 %u/%u 块, 校验并结束\n",
                      (unsigned)_ota_expect_seq, (unsigned)_ota_total_seq);
        if (Update.end(true)) {
            Serial.println(F("[OTA] 校验通过, 升级成功, 即将重启"));
            otaSendAck("OTAOK");
            c3OtaProgress.percent = 100;
            // 稍延迟让 ACK 发出, 再重启切到新槽
            delay(500);
            ESP.restart();
        } else {
            Serial.print(F("[OTA] ERROR: Update.end 失败: "));
            Update.printError(Serial);
            otaSendAck(("OTAFAIL," + String(Update.getError())).c_str());
            Update.abort();
            _ota_active = false;
            c3OtaProgress.active = false;
        }
    }
};

extern UartLink netLink;   // 沿用 netLink 名, c3_terminal.ino 无需改引用
C3OtaProgress c3OtaProgress;  // 全局实例 (本头仅被 c3_terminal.ino 包含一次)
