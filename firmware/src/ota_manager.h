// ============================================================
//  ota_manager.h - OTA 升级管理 (主控自更新 + C3 固件暂存/转发)
//
//  功能分两块:
//    1) 主控固件: 手机上传 firmware.bin → Update 写入对面 OTA 槽 → 重启
//    2) C3 终端固件: 手机上传 c3.bin → 暂存 SPIFFS → UART1 转发到 C3
//
//  安全网:
//    - 分区表 default.csv 已含 ota_0/ota_1 + otadata (A/B 双槽)
//    - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1: 新固件 panic/WDT 重启 →
//      bootloader 自动回滚到上一好槽 (永不真砖)
//    - Update.end(true) 自带 SHA-256 校验, bootloader 启动时二次校验
//
//  看门狗风险 (主控 5s TWDT):
//    - setup() 已有 esp_task_wdt_deinit()/reinit 模板 (ebike_bsd.ino:122,166)
//    - Upload 回调跑在 AsyncTCP 任务上, loopTask 仍每 50ms 喂狗
//    - Update.end() 前后按上述模式包一层, 杜绝写 Flash 期间 5s 超时重启
// ============================================================
#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <Update.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// ==== 外部依赖 (定义在 ebike_bsd.ino / wifi_web.h / terminal_link.h) ====
extern AsyncWebServer server;
extern bool g_wifi_running;
extern unsigned long g_wifi_idle_since;
extern HardwareSerial Serial1;   // UART1 → C3 终端 (terminal_link.h)

// C3 固件暂存路径 (主控 spiffs 分区 ~1.375MB, 当前完全空闲)
#define OTA_C3_FW_PATH    "/c3fw.bin"
#define OTA_C3_META_PATH  "/c3meta.txt"   // 记录 size/version, 供状态查询

// C3 转发分块大小 (裸字节数, 编码成 hex 后翻倍)
#define OTA_BLOCK_BYTES   128
#define OTA_BLOCK_TIMEOUT_MS   1000   // 单块等 ACK 超时
#define OTA_BEGIN_TIMEOUT_MS   2000   // $OTAB 等 ready 超时
#define OTA_END_TIMEOUT_MS    15000   // $OTAE 等 OK/FAIL 超时
#define OTA_MAX_RETRIES           3   // 单块最多重传次数

// ============================================================
//  OTA 状态 (供 Web UI 轮询显示进度)
// ============================================================
enum OtaMainState {
    OTA_MAIN_IDLE = 0,
    OTA_MAIN_UPLOADING,
    OTA_MAIN_SUCCESS,     // 已完成, 等待重启
    OTA_MAIN_FAILED
};

enum OtaC3State {
    OTA_C3_IDLE = 0,
    OTA_C3_UPLOADING,     // 正在接收手机上传到 SPIFFS
    OTA_C3_STAGED,        // 已暂存, 等待转发
    OTA_C3_RELAYING,      // 正在经 UART1 转发到 C3
    OTA_C3_SUCCESS,
    OTA_C3_FAILED
};

struct OtaStatus {
    // 主控
    OtaMainState main_state = OTA_MAIN_IDLE;
    size_t main_written = 0;        // 已写入字节
    size_t main_total = 0;
    String main_error;              // 失败原因
    unsigned long main_reboot_at = 0;  // 计划重启时间 (0=未安排)

    // C3
    OtaC3State c3_state = OTA_C3_IDLE;
    size_t c3_staged_size = 0;      // SPIFFS 暂存的固件大小
    String c3_staged_version;
    size_t c3_relay_seq = 0;        // 当前转发块序号
    size_t c3_relay_total = 0;      // 总块数
    String c3_error;
};
// 全局实例 (本头仅被 ebike_bsd.ino 包含一次, 故在此定义而非 extern)
OtaStatus otaStatus;

// 运行槽信息 (ota_0/ota_1/factory), 供 Web UI 显示
inline const char* otaGetRunningSlot() {
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return "?";
    return p->label;
}

// 是否刚从 OTA 更新启动 (本槽待确认), 用于 setup() 末尾调 mark_valid
// NEW 和 PENDING_VERIFY 都表示"尚未确认", 首次启动是 NEW, 后续重启 bootloader 标记为 PENDING_VERIFY
inline bool otaIsPendingVerify() {
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW;
}

// ============================================================
//  阻止 Arduino-ESP32 core 自动确认新 OTA 固件
//
//  Arduino core 的 initArduino() (在 app_main 里, 早于 setup) 默认会调
//  esp_ota_mark_app_valid_cancel_rollback() 把新槽直接标记 VALID, 绕过 pending_verify.
//  这会让 bootloader 回滚机制 + 我们的 boot guard 全部失效 (实测确认).
//
//  这两个函数是 __attribute__((weak)), 这里覆盖:
//    verifyRollbackLater() = true  → 告诉 core "我要稍后自己验证, 别自动确认"
//    verifyOta()           = false → 双保险: 即使 core 想确认, 也判定为不通过 → 它会自动回滚
//
//  ⚠ verifyOta 返回 false 会让 core 主动 esp_ota_mark_invalid_rollback_and_reboot(),
//     所以必须配合 verifyRollbackLater()=true 阻止 core 介入, 由我们的 otaBootGuardBegin()
//     统一处理 (允许 N 次尝试, 不至于首启 panic 就立即回滚).
// ============================================================
extern "C" {
    bool verifyRollbackLater() { return true; }   // 应用层稍后验证, 阻止 core 自动确认
}

// ============================================================
//  应用层主动回滚保护 (补足 bootloader 默认不自动回滚的缺口)
//
//  背景: ESP-IDF 的 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 默认行为下,
//        新 OTA app panic/WDT 重启后 bootloader 不会自动回滚 (需 app 主动调
//        mark_invalid). 纯靠 bootloader 计数需要改 sdkconfig, 这里用 NVS 计数兜底.
//
//  原理: setup() 最开始调 otaBootGuardBegin() 记一次"尝试启动" (NVS 计数+1).
//        若 setup() 正常跑完 (到 otaMarkValid), 计数清零.
//        若 setup() 中途 panic/重启, 下次启动计数又+1; 超过阈值 → 主动回滚.
// ============================================================
#define OTA_BOOTGUARD_NS    "ota_guard"
#define OTA_BOOTGUARD_KEY   "bootfails"
#define OTA_BOOTGUARD_MAX      3   // 连续 3 次 setup 未完成 → 判定坏固件, 回滚

// setup() 开头调用: 若本槽待确认, 记一次失败尝试; 超阈值则强制回滚
inline void otaBootGuardBegin() {
    if (!otaIsPendingVerify()) return;   // 已确认过的好槽 (UNDEFINED/VALID), 不计数

    Preferences prefs;
    if (!prefs.begin(OTA_BOOTGUARD_NS, false)) return;
    int fails = prefs.getInt(OTA_BOOTGUARD_KEY, 0) + 1;
    prefs.putInt(OTA_BOOTGUARD_KEY, fails);
    prefs.end();

    Serial.printf("[OTA-GUARD] 新固件第 %d/%d 次尝试启动\n", fails, OTA_BOOTGUARD_MAX);
    if (fails >= OTA_BOOTGUARD_MAX) {
        Serial.println(F("[OTA-GUARD] 连续启动失败超阈值 → 强制回滚到上一好槽"));
        if (prefs.begin(OTA_BOOTGUARD_NS, false)) {
            prefs.putInt(OTA_BOOTGUARD_KEY, 0);
            prefs.end();
        }
        Serial.flush();
        delay(100);
        esp_ota_mark_app_invalid_rollback_and_reboot();   // 不会返回
    }
}

// 确认本槽健康 (setup 末尾调, 取消回滚挂起 + 清失败计数)
inline void otaMarkValid() {
    if (otaIsPendingVerify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println(F("[OTA] 本槽已标记 valid (取消回滚挂起)"));
        // 清 boot guard 失败计数
        Preferences prefs;
        if (prefs.begin(OTA_BOOTGUARD_NS, false)) {
            prefs.putInt(OTA_BOOTGUARD_KEY, 0);
            prefs.end();
        }
    }
}

// ============================================================
//  CRC16-CCITT (用于 UART 转发分块校验, 与 uart_link.h C3 端一致)
// ============================================================
inline uint16_t otaCrc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

// ============================================================
//  WiFi 保活 / 功率提升 (OTA 期间避免 30s 自动关, 提高吞吐)
// ============================================================
inline void otaKeepWifiAlive() {
    g_wifi_running = true;
    g_wifi_idle_since = 0;   // 重置 30s 空闲计时
}

inline void otaBoostTxPower() {
    // 上传期间临时提升发射功率 (日常 11dBm 降功耗, OTA 期间换吞吐)
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
}

inline void otaRestoreTxPower() {
    WiFi.setTxPower(WIFI_POWER_11dBm);
}

// ============================================================
//  主控固件上传: AsyncWebServer multipart handler
//  用法: server.on("/api/ota_main", HTTP_POST, onUploadDone, onUpload);
//    onUpload(request, filename, index, data, len, final)
// ============================================================
inline void otaMainUploadHandler(AsyncWebServerRequest *req, const String& filename,
                                  size_t index, uint8_t *data, size_t len, bool final) {
    otaKeepWifiAlive();

    if (index == 0) {
        // 首块: 初始化
        otaBoostTxPower();
        otaStatus.main_state = OTA_MAIN_UPLOADING;
        otaStatus.main_written = 0;
        otaStatus.main_total = req->contentLength();
        otaStatus.main_error = "";
        otaStatus.main_reboot_at = 0;
        Serial.printf("[OTA] 主控上传开始: %s (%u bytes)\n",
                      filename.c_str(), (unsigned)otaStatus.main_total);

        // 暂停任务看门狗 (Update.end 可能较长; 与 setup() 模式一致)
        esp_task_wdt_deinit();

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaStatus.main_state = OTA_MAIN_FAILED;
            otaStatus.main_error = "Update.begin failed";
            Serial.println(F("[OTA] ERROR: Update.begin failed"));
            esp_task_wdt_init(5, true);
            esp_task_wdt_add(NULL);
            return;
        }
    }

    // 写入数据块
    if (len > 0 && otaStatus.main_state == OTA_MAIN_UPLOADING) {
        size_t w = Update.write(data, len);
        if (w != len) {
            otaStatus.main_state = OTA_MAIN_FAILED;
            otaStatus.main_error = "write mismatch";
            Serial.printf("[OTA] ERROR: write %u != %u\n", (unsigned)w, (unsigned)len);
        } else {
            otaStatus.main_written += w;
        }
    }

    if (final) {
        if (otaStatus.main_state == OTA_MAIN_UPLOADING) {
            // 校验 + 完成写入 (true=校验通过才切换槽)
            if (Update.end(true)) {
                Serial.printf("[OTA] 主控上传完成: %u bytes, 校验通过, 即将重启\n",
                              (unsigned)otaStatus.main_written);
                otaStatus.main_state = OTA_MAIN_SUCCESS;
                otaStatus.main_reboot_at = millis() + 1500;  // 1.5s 后重启 (让响应先发出)
            } else {
                otaStatus.main_state = OTA_MAIN_FAILED;
                // Arduino-ESP32 Update 错误 API: getError() 返回码, printError(Serial) 打印详情
                otaStatus.main_error = String("end/verify err=") + Update.getError();
                Serial.print(F("[OTA] ERROR: Update.end failed: "));
                Update.printError(Serial);
                Update.abort();
            }
        }
        // 恢复看门狗 (即使失败也恢复, 让 loop 正常运行)
        esp_task_wdt_init(5, true);
        esp_task_wdt_add(NULL);
        otaRestoreTxPower();
    }
}

// 上传完成后的 HTTP 响应 (POST 主 handler, 空 onBody)
inline void otaMainUploadDone(AsyncWebServerRequest *req) {
    AsyncWebServerResponse *resp;
    if (otaStatus.main_state == OTA_MAIN_SUCCESS) {
        resp = req->beginResponse(200, "application/json",
            F("{\"ok\":true,\"msg\":\"upload ok, rebooting\"}"));
    } else {
        resp = req->beginResponse(400, "application/json",
            String(F("{\"ok\":false,\"msg\":\"")) + otaStatus.main_error + F("\"}"));
    }
    resp->addHeader("Access-Control-Allow-Origin", "*");
    req->send(resp);
}

// 在 loop() 中调用: 处理上传成功后的延迟重启
inline void otaMainLoopTick() {
    if (otaStatus.main_state == OTA_MAIN_SUCCESS && otaStatus.main_reboot_at &&
        millis() >= otaStatus.main_reboot_at) {
        Serial.println(F("[OTA] 重启中..."));
        Serial.flush();
        delay(100);
        ESP.restart();
    }
}

// ============================================================
//  C3 固件暂存: 手机上传 c3.bin → SPIFFS
// ============================================================
inline void otaC3UploadHandler(AsyncWebServerRequest *req, const String& filename,
                                size_t index, uint8_t *data, size_t len, bool final) {
    otaKeepWifiAlive();

    if (index == 0) {
        otaBoostTxPower();
        otaStatus.c3_state = OTA_C3_UPLOADING;
        otaStatus.c3_staged_size = 0;
        otaStatus.c3_error = "";
        // 删除可能存在的旧暂存
        SPIFFS.remove(OTA_C3_FW_PATH);
        SPIFFS.remove(OTA_C3_META_PATH);
        Serial.printf("[OTA] C3 固件上传开始: %s\n", filename.c_str());
    }

    if (len > 0 && otaStatus.c3_state == OTA_C3_UPLOADING) {
        // 追加写 (O_WRITE | O_APPEND | O_CREAT)
        File f = SPIFFS.open(OTA_C3_FW_PATH, "a");
        if (!f) {
            otaStatus.c3_state = OTA_C3_FAILED;
            otaStatus.c3_error = "SPIFFS write open failed";
            Serial.println(F("[OTA] ERROR: SPIFFS 打开失败"));
            return;
        }
        size_t w = f.write(data, len);
        f.close();
        if (w != len) {
            otaStatus.c3_state = OTA_C3_FAILED;
            otaStatus.c3_error = "SPIFFS write short";
            Serial.printf("[OTA] ERROR: SPIFFS write %u != %u\n", (unsigned)w, (unsigned)len);
            return;
        }
        otaStatus.c3_staged_size += w;
    }

    if (final) {
        otaRestoreTxPower();
        if (otaStatus.c3_state == OTA_C3_UPLOADING) {
            // 记录元信息 (供 /api/ota_c3_status 查询, 也作为"有待转发"标志)
            File m = SPIFFS.open(OTA_C3_META_PATH, "w");
            if (m) {
                m.printf("%u\n", (unsigned)otaStatus.c3_staged_size);
                m.close();
            }
            otaStatus.c3_state = OTA_C3_STAGED;
            Serial.printf("[OTA] C3 固件已暂存到 SPIFFS: %u bytes, 等待转发\n",
                          (unsigned)otaStatus.c3_staged_size);
        }
    }
}

inline void otaC3UploadDone(AsyncWebServerRequest *req) {
    AsyncWebServerResponse *resp;
    if (otaStatus.c3_state == OTA_C3_STAGED) {
        resp = req->beginResponse(200, "application/json",
            F("{\"ok\":true,\"msg\":\"staged, relaying to C3\"}"));
    } else {
        resp = req->beginResponse(400, "application/json",
            String(F("{\"ok\":false,\"msg\":\"")) + otaStatus.c3_error + F("\"}"));
    }
    resp->addHeader("Access-Control-Allow-Origin", "*");
    req->send(resp);
}

// ============================================================
//  C3 转发状态机 (主控 → C3, 经 UART1)
//
//  协议帧 (ASCII 行, 与 terminal_link.h/uart_link.h 风格一致):
//    主控→C3:  $OTAB,<size>,<version>\n           (开始)
//              $OTAC,<seq>,<hex>,<crc16>\n        (分块, hex 为 OTA_BLOCK_BYTES 字节的十六进制)
//              $OTAE\n                            (结束)
//    C3→主控:  $C,OTAR,ready\n / $C,OTAR,<seq>\n  (ACK)
//              $C,OTAN,<seq>\n                    (NACK, 重传)
//              $C,OTAOK\n / $C,OTAFAIL,<reason>\n (结束)
//
//  这些 OTA 帧由 terminal_link.h 的 applyCommand() 分发到下面的接口.
// ============================================================

struct OtaRelayCtx {
    bool active = false;
    size_t total = 0;
    size_t totalSeq = 0;
    size_t curSeq = 0;
    File fwFile;
    // 当前块等待 ACK 的状态
    bool waitingAck = false;
    unsigned long sentAt = 0;
    int retries = 0;
    uint8_t blockBuf[OTA_BLOCK_BYTES];
    size_t lastSentLen = 0;     // 最近发送块的实际字节数 (重传时直接复用 blockBuf, 不重读文件)
    // begin/end 阶段
    bool beginSent = false;
    bool endSent = false;
    unsigned long beginSentAt = 0;
    unsigned long endSentAt = 0;
};
static OtaRelayCtx _relay;

// ---- 由 terminal_link.h applyCommand 调用: C3 回复到达 ----
inline void otaRelayOnC3Ready() {
    if (_relay.active && !_relay.beginSent) {
        // 收到 OTAR,ready → 标记 begin 完成
        _relay.beginSent = true;
        Serial.println(F("[OTA-RELAY] C3 ready, 开始流式发送"));
    }
}

inline void otaRelayOnAck(uint32_t seq) {
    if (!_relay.active) return;
    if (seq != _relay.curSeq) return;   // 过期 ACK 忽略
    _relay.waitingAck = false;
    _relay.curSeq++;
    _relay.retries = 0;
    _relay.lastSentLen = 0;   // 清除重传缓存, 下一块会从文件读新数据
    otaStatus.c3_relay_seq = _relay.curSeq;
}

inline void otaRelayOnNack(uint32_t seq) {
    if (!_relay.active) return;
    if (seq != _relay.curSeq) return;
    _relay.waitingAck = false;   // 触发立即重传
}

inline void otaRelayOnOk() {
    if (!_relay.active) return;
    Serial.println(F("[OTA-RELAY] C3 升级成功"));
    otaStatus.c3_state = OTA_C3_SUCCESS;
    _relay.active = false;
    _relay.fwFile.close();
    SPIFFS.remove(OTA_C3_FW_PATH);
    SPIFFS.remove(OTA_C3_META_PATH);
}

inline void otaRelayOnFail(const String& reason) {
    if (!_relay.active) return;
    Serial.printf("[OTA-RELAY] C3 升级失败: %s\n", reason.c_str());
    otaStatus.c3_state = OTA_C3_FAILED;
    otaStatus.c3_error = reason;
    _relay.active = false;
    _relay.fwFile.close();
    // 保留 SPIFFS 暂存, 允许重试
}

// ---- 发送一帧到 C3 ----
inline void otaSendBegin(size_t size, const char* version) {
    char buf[80];
    snprintf(buf, sizeof(buf), "$OTAB,%u,%s\n", (unsigned)size, version);
    Serial1.print(buf);
}

inline void otaSendChunk(uint32_t seq, const uint8_t *data, size_t len) {
    // $OTAC,<seq>,<hex(2*len)>,<crc16>\n
    // 缓冲: 帧头 ~16 + hex 2*128=256 + 尾 ~8 = ~280
    char buf[320];
    int pos = snprintf(buf, sizeof(buf), "$OTAC,%u,", (unsigned)seq);
    static const char hexchars[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len && pos + 3 < (int)sizeof(buf); i++) {
        buf[pos++] = hexchars[data[i] >> 4];
        buf[pos++] = hexchars[data[i] & 0x0F];
    }
    uint16_t crc = otaCrc16(data, len);
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",%u\n", (unsigned)crc);
    Serial1.write((const uint8_t*)buf, pos);
}

inline void otaSendEnd() {
    Serial1.print(F("$OTAE\n"));
}

// ---- 启动转发 (从 SPIFFS 读已暂存固件) ----
inline void otaRelayStart() {
    if (_relay.active) return;
    if (otaStatus.c3_state != OTA_C3_STAGED) return;
    _relay.active = true;
    _relay.total = otaStatus.c3_staged_size;
    _relay.totalSeq = (_relay.total + OTA_BLOCK_BYTES - 1) / OTA_BLOCK_BYTES;
    _relay.curSeq = 0;
    _relay.retries = 0;
    _relay.waitingAck = false;
    _relay.beginSent = false;
    _relay.endSent = false;
    _relay.fwFile = SPIFFS.open(OTA_C3_FW_PATH, "r");
    if (!_relay.fwFile) {
        otaRelayOnFail("SPIFFS open read failed");
        return;
    }
    otaStatus.c3_state = OTA_C3_RELAYING;
    otaStatus.c3_relay_seq = 0;
    otaStatus.c3_relay_total = _relay.totalSeq;
    otaStatus.c3_error = "";
    Serial.printf("[OTA-RELAY] 启动: %u bytes / %u 块\n",
                  (unsigned)_relay.total, (unsigned)_relay.totalSeq);
    otaSendBegin(_relay.total, otaStatus.c3_staged_version.c_str());
    _relay.beginSentAt = millis();
}

// 手动触发重试 (Web UI 按钮: 重新转发上次失败的 C3 固件)
inline void otaRelayRetry() {
    if (_relay.active) return;
    if (!SPIFFS.exists(OTA_C3_META_PATH)) return;
    // 重读元信息
    File m = SPIFFS.open(OTA_C3_META_PATH, "r");
    if (m) {
        otaStatus.c3_staged_size = m.parseInt();
        m.close();
    }
    if (otaStatus.c3_staged_size == 0) return;
    otaStatus.c3_state = OTA_C3_STAGED;
    otaStatus.c3_error = "";
}

// ---- loop() 调用: 驱动转发状态机 ----
inline void otaRelayStep() {
    if (!_relay.active) {
        // 空闲时检查是否有暂存待转发
        if (otaStatus.c3_state == OTA_C3_STAGED) {
            otaRelayStart();
        }
        return;
    }

    unsigned long now = millis();

    // 阶段 1: 等 C3 ready (OTAR,ready)
    if (!_relay.beginSent) {
        if (now - _relay.beginSentAt > OTA_BEGIN_TIMEOUT_MS) {
            Serial.println(F("[OTA-RELAY] begin 超时, 重发 $OTAB"));
            otaSendBegin(_relay.total, otaStatus.c3_staged_version.c_str());
            _relay.beginSentAt = now;
        }
        return;
    }

    // 阶段 2: 流式发送分块
    if (!_relay.endSent) {
        if (!_relay.waitingAck) {
            if (_relay.curSeq >= _relay.totalSeq) {
                // 全部发送完 → 发 $OTAE
                otaSendEnd();
                _relay.endSent = true;
                _relay.endSentAt = now;
                Serial.println(F("[OTA-RELAY] 全部块发送完, 发 $OTAE"));
                return;
            }
            // 读下一块 (或重传上一块: retries>0 表示上次发送未被 ACK, 直接复用 blockBuf)
            size_t got;
            if (_relay.retries > 0 && _relay.lastSentLen > 0) {
                // 重传: 不重读文件 (文件游标已推进), 复用上次发送的 blockBuf
                got = _relay.lastSentLen;
                Serial.printf("[OTA-RELAY] 重传块 %u (%d/%d)\n",
                              (unsigned)_relay.curSeq, _relay.retries, OTA_MAX_RETRIES);
            } else {
                // 新块: 顺序读 (文件游标由 read 自动推进)
                got = _relay.fwFile.read(_relay.blockBuf, OTA_BLOCK_BYTES);
                if (got == 0) {
                    otaRelayOnFail("SPIFFS read 0");
                    return;
                }
                _relay.lastSentLen = got;
            }
            otaSendChunk(_relay.curSeq, _relay.blockBuf, got);
            _relay.waitingAck = true;
            _relay.sentAt = now;
        } else {
            // 等 ACK
            if (now - _relay.sentAt > OTA_BLOCK_TIMEOUT_MS) {
                _relay.retries++;
                if (_relay.retries > OTA_MAX_RETRIES) {
                    otaRelayOnFail("block timeout, retries exceeded");
                    return;
                }
                _relay.waitingAck = false;   // 落到上面分支 (retries>0 会复用 blockBuf 重传)
            }
        }
        return;
    }

    // 阶段 3: 等 OTAOK/OTAFAIL
    if (now - _relay.endSentAt > OTA_END_TIMEOUT_MS) {
        otaRelayOnFail("end timeout");
    }
}

// ============================================================
//  状态查询 JSON (GET /api/ota_status)
// ============================================================
inline void otaStatusHandler(AsyncWebServerRequest *req) {
    String j = "{";
    // 主控
    const char* ms;
    switch (otaStatus.main_state) {
        case OTA_MAIN_IDLE: ms="idle"; break;
        case OTA_MAIN_UPLOADING: ms="uploading"; break;
        case OTA_MAIN_SUCCESS: ms="success"; break;
        case OTA_MAIN_FAILED: ms="failed"; break;
        default: ms="?";
    }
    j += "\"main\":{\"state\":\""; j += ms;
    j += "\",\"written\":"; j += otaStatus.main_written;
    j += ",\"total\":"; j += otaStatus.main_total;
    j += ",\"slot\":\""; j += otaGetRunningSlot(); j += "\"";
    j += ",\"version\":\""; j += FW_VERSION; j += "\"";
    if (otaStatus.main_error.length()) { j += ",\"error\":\""; j += otaStatus.main_error; j += "\""; }
    j += "},";
    // C3
    const char* cs;
    switch (otaStatus.c3_state) {
        case OTA_C3_IDLE: cs="idle"; break;
        case OTA_C3_UPLOADING: cs="uploading"; break;
        case OTA_C3_STAGED: cs="staged"; break;
        case OTA_C3_RELAYING: cs="relaying"; break;
        case OTA_C3_SUCCESS: cs="success"; break;
        case OTA_C3_FAILED: cs="failed"; break;
        default: cs="?";
    }
    j += "\"c3\":{\"state\":\""; j += cs;
    j += "\",\"seq\":"; j += otaStatus.c3_relay_seq;
    j += ",\"total\":"; j += otaStatus.c3_relay_total;
    j += ",\"staged_size\":"; j += otaStatus.c3_staged_size;
    if (otaStatus.c3_error.length()) { j += ",\"error\":\""; j += otaStatus.c3_error; j += "\""; }
    j += "}}";
    AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", j);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    req->send(resp);
}

// ============================================================
//  路由注册 (initWebServer 调用)
// ============================================================
inline void otaRegisterRoutes() {
    // 主控固件上传 (multipart: 字段名任意, 文件即 firmware.bin)
    server.on("/api/ota_main", HTTP_POST, otaMainUploadDone, otaMainUploadHandler);
    // C3 固件上传到 SPIFFS 暂存
    server.on("/api/ota_c3", HTTP_POST, otaC3UploadDone, otaC3UploadHandler);
    // 状态查询 (主控+C3 一起)
    server.on("/api/ota_status", HTTP_GET, otaStatusHandler);
    // 手动重试转发失败的 C3 固件
    server.on("/api/ota_c3_retry", HTTP_POST, [](AsyncWebServerRequest *req){
        otaRelayRetry();
        req->send(200, "application/json", F("{\"ok\":true}"));
    });
}

#endif // OTA_MANAGER_H
