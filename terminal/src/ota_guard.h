// ============================================================
//  ota_guard.h - C3 OTA 回滚保护
//
//  从 uart_link.h 拆出, 职责单一: 阻止 Arduino core 自动确认新 OTA 固件 +
//  应用层 boot guard (NVS 计数, 连续 N 次 setup 未完成则强制回滚).
//
//  不依赖 UartLink 类, 可独立 include.
// ============================================================
#ifndef C3_OTA_GUARD_H
#define C3_OTA_GUARD_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// 是否刚从 OTA 升级启动 (本槽待确认), setup() 末尾调 mark_valid
// NEW 和 PENDING_VERIFY 都表示"尚未确认"
inline bool c3OtaIsPendingVerify() {
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW;
}

// 确认本槽健康 (setup 末尾调, 取消回滚挂起 + 清失败计数)
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
//  阻止 Arduino-ESP32 core 自动确认新 OTA 固件
//  Arduino core 的 initArduino() 默认调 mark_valid, 覆盖 weak 阻止.
// ============================================================
extern "C" {
    bool verifyRollbackLater() { return true; }
}

// ============================================================
//  应用层主动回滚保护 (NVS 计数)
// ============================================================
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

#endif // C3_OTA_GUARD_H
