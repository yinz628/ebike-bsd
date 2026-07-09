// ============================================================
//  config_store.h - 配置存储 (JSON + NVS)
//  V2.5+: 所有 #define 参数迁移到结构体, JSON序列化到NVS
// ============================================================
#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ============ 配置结构体 ============

struct RCWParams {
    int left_angle_min  = -40;     // °, 左检测区起始
    int left_angle_max  = -5;      // °, 左检测区结束
    int right_angle_min = 5;       // °, 右检测区起始
    int right_angle_max = 40;      // °, 右检测区结束
    int low_speed       = 2;       // m/s, 低警告阈值
    int speed_threshold = 3;       // m/s, 高警告阈值
    int range_limit     = 25;      // m
    int lateral_limit   = 3;       // m, 横向距离上限 (过滤边缘远处车辆误报)
    int hold_time       = 3000;    // ms
    int flash_interval  = 125;     // ms, 高LED半周期 (4Hz)
    int lflash_interval = 500;     // ms, 低LED半周期 (1Hz)
};

struct TurnAssistParams {
    int left_angle_min  = -40;
    int left_angle_max  = -5;
    int right_angle_min = 5;
    int right_angle_max = 40;
    int speed_threshold = 2;       // m/s
    int range_limit     = 30;      // m
    int lateral_limit   = 3;       // m, 横向距离上限 (同 RCW, 过滤远处车)
};

struct SystemParams {
    char wifi_ssid[32]     = "eBike-BSD";
    char wifi_pass[32]     = "12345678";
    int  led_brightness    = 255;
    int  bsd_beep_cooldown = 5000;
};

struct RadarParams {
    int baud_rate     = 921600;  // UART波特率
    int det_range     = 30;      // 最大探测距离 (m)
    int sensitivity   = 2;       // 灵敏度 (0-10, 越小越灵敏)
};

// ============ 全局配置类 ============
class ConfigStore {
public:
    RCWParams       rcw;
    TurnAssistParams turn;
    SystemParams     sys;
    RadarParams      radar;

    // ---- JSON 序列化 ----
    void toJson(JsonDocument &doc) {
        JsonObject r = doc["rcw"].to<JsonObject>();
        r["left_min"]  = rcw.left_angle_min;
        r["left_max"]  = rcw.left_angle_max;
        r["right_min"] = rcw.right_angle_min;
        r["right_max"] = rcw.right_angle_max;
        r["low_speed"] = rcw.low_speed;
        r["speed"]     = rcw.speed_threshold;
        r["range"]     = rcw.range_limit;
        r["lateral"]   = rcw.lateral_limit;
        r["hold"]      = rcw.hold_time;
        r["lflash"]    = rcw.lflash_interval;
        r["flash"]     = rcw.flash_interval;

        JsonObject t = doc["turn"].to<JsonObject>();
        t["left_min"]  = turn.left_angle_min;
        t["left_max"]  = turn.left_angle_max;
        t["right_min"] = turn.right_angle_min;
        t["right_max"] = turn.right_angle_max;
        t["speed"]     = turn.speed_threshold;
        t["range"]     = turn.range_limit;
        t["lateral"]   = turn.lateral_limit;

        JsonObject s = doc["sys"].to<JsonObject>();
        s["wifi_ssid"] = sys.wifi_ssid;
        s["bsd_beep_cooldown"] = sys.bsd_beep_cooldown;
        s["led_brightness"] = sys.led_brightness;

        JsonObject rd = doc["radar"].to<JsonObject>();
        rd["det_range"]   = radar.det_range;
        rd["sensitivity"] = radar.sensitivity;
    }

    void fromJson(JsonDocument &doc) {
        JsonObject r = doc["rcw"];
        if (r) {
            rcw.left_angle_min  = r["left_min"]  | rcw.left_angle_min;
            rcw.left_angle_max  = r["left_max"]  | rcw.left_angle_max;
            rcw.right_angle_min = r["right_min"] | rcw.right_angle_min;
            rcw.right_angle_max = r["right_max"] | rcw.right_angle_max;
            rcw.low_speed       = r["low_speed"] | rcw.low_speed;
            rcw.speed_threshold = r["speed"] | rcw.speed_threshold;
            rcw.range_limit     = r["range"] | rcw.range_limit;
            rcw.lateral_limit   = r["lateral"] | rcw.lateral_limit;
            rcw.hold_time       = r["hold"]  | rcw.hold_time;
            rcw.lflash_interval = r["lflash"] | rcw.lflash_interval;
            rcw.flash_interval  = r["flash"] | rcw.flash_interval;
        }
        JsonObject t = doc["turn"];
        if (t) {
            turn.left_angle_min  = t["left_min"]  | turn.left_angle_min;
            turn.left_angle_max  = t["left_max"]  | turn.left_angle_max;
            turn.right_angle_min = t["right_min"] | turn.right_angle_min;
            turn.right_angle_max = t["right_max"] | turn.right_angle_max;
            turn.speed_threshold = t["speed"]     | turn.speed_threshold;
            turn.range_limit     = t["range"]     | turn.range_limit;
            turn.lateral_limit   = t["lateral"]   | turn.lateral_limit;
        }
        JsonObject s = doc["sys"];
        if (s) {
            if (s["wifi_ssid"]) strlcpy(sys.wifi_ssid, s["wifi_ssid"], sizeof(sys.wifi_ssid));
            sys.bsd_beep_cooldown = s["bsd_beep_cooldown"] | sys.bsd_beep_cooldown;
        }
        JsonObject rd = doc["radar"];
        if (rd) {
            radar.det_range   = rd["det_range"]   | radar.det_range;
            radar.sensitivity = rd["sensitivity"] | radar.sensitivity;
        }

    }

    // ---- NVS 持久化 ----
    bool loadFromNVS() {
        Preferences prefs;
        if (!prefs.begin("ebike", true)) return false;
        String json = prefs.getString("config", "");
        prefs.end();
        if (json.isEmpty()) return false;
        static StaticJsonDocument<4096> doc;
        DeserializationError err = deserializeJson(doc, json);
        if (err) {
            Serial.print("[CONFIG] JSON err: ");
            Serial.println(err.c_str());
            return false;
        }
        fromJson(doc);
        Serial.println("[CONFIG] loaded from NVS");
        return true;
    }

    bool saveToNVS() {
        Preferences prefs;
        if (!prefs.begin("ebike", false)) {
            Serial.println("[CONFIG] ERROR: NVS open failed!");
            return false;
        }
        static StaticJsonDocument<4096> doc;
        doc.clear();
        toJson(doc);
        String json;
        serializeJson(doc, json);
        Serial.print("[CONFIG] saving JSON ("); Serial.print(json.length()); Serial.print("B): ");
        Serial.println(json);
        size_t written = prefs.putString("config", json);
        prefs.end();
        if (written == json.length()) {
            Serial.println("[CONFIG] saved to NVS OK");
            return true;
        } else {
            Serial.print("[CONFIG] ERROR: NVS write failed! wrote=");
            Serial.print(written); Serial.print(" expected=");
            Serial.println(json.length());
            return false;
        }
    }

    void summary() {
        Serial.print("[CONFIG] RCW: low=");
        Serial.print(rcw.low_speed); Serial.print("m/s speed=");
        Serial.print(rcw.speed_threshold); Serial.print("m/s range=");
        Serial.print(rcw.range_limit); Serial.print("m hold=");
        Serial.println(rcw.hold_time);
        Serial.print("[CONFIG] Radar: range=");
        Serial.print(radar.det_range); Serial.print("m sens=");
        Serial.println(radar.sensitivity);
    }

    void factoryReset() {
        // 重置内存结构体为默认值
        rcw   = RCWParams();
        turn  = TurnAssistParams();
        sys   = SystemParams();
        radar = RadarParams();
        // 清空 NVS 中的旧配置
        // ⚠ 不可用 esp_partition_erase_region(0x9000, 0x6000) 之类裸擦除:
        //    会越界破坏 otadata 分区 → 下次 NVS 初始化失败 → loadFromNVS 返回 false
        //    → 若 setup 里"失败即写默认值"则用户配置被永久覆盖.
        //    只用 Preferences::clear() 精确清理本命名空间.
        Preferences prefs;
        if (prefs.begin("ebike", false)) {
            prefs.clear();   // 仅清 ebike 命名空间, 不影响其他分区
            prefs.end();
        }
        // 写入默认配置
        saveToNVS();
    }
};

extern ConfigStore config;

// ============ 角度判断内联函数 (运行时从config读取) ============
// 后方监测角度 (BSD+RCW合并, 使用 config.rcw 参数)
inline bool ANGLE_IS_LEFT(int a) {
    return a >= config.rcw.left_angle_min && a <= config.rcw.left_angle_max;
}
inline bool ANGLE_IS_RIGHT(int a) {
    return a >= config.rcw.right_angle_min && a <= config.rcw.right_angle_max;
}
inline bool ANGLE_IS_CENTER(int a) {
    return a > config.rcw.left_angle_max && a < config.rcw.right_angle_min;
}

// 转向辅助角度
inline bool TURN_ANGLE_IS_LEFT(int a) {
    return a >= config.turn.left_angle_min && a <= config.turn.left_angle_max;
}
inline bool TURN_ANGLE_IS_RIGHT(int a) {
    return a >= config.turn.right_angle_min && a <= config.turn.right_angle_max;
}

// 后方监测别名 (向后兼容)
inline bool REAR_ANGLE_IS_LEFT(int a)  { return ANGLE_IS_LEFT(a); }
inline bool REAR_ANGLE_IS_RIGHT(int a) { return ANGLE_IS_RIGHT(a); }

#endif // CONFIG_STORE_H
