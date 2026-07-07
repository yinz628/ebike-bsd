// ============================================================
//  net_link.h - C3 终端 WiFi 通信 (与主控 web API 对接)
//  C3 作为 STA 连主控 AP (eBike-BSD), HTTP GET /api/status 取状态
//  触摸配置 → HTTP POST /api/config
//  延迟 ~200ms, 对显示足够; 报警音同步用 buzzer 字段
// ============================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#define MASTER_SSID   "eBike-BSD"
#define MASTER_PASS   "12345678"
#define MASTER_HOST   "http://192.168.4.1"
#define POLL_INTERVAL_MS  200   // 5Hz 轮询

// ============ 接收到的状态 (与 uart_link.h 一致) ============
struct TermObj {
    int8_t   range;
    int8_t   angle;
    int8_t   velocity;
    uint8_t  id;
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

class NetLink {
private:
    WiFiClient _client;
    HTTPClient _http;
    unsigned long _last_poll;
    bool _wifi_connected;
    String _status_buf;   // JSON 累积缓冲

    // 极简 JSON 值提取 (避免引入 ArduinoJson 到 C3 省 Flash)
    // 在 json 里找 "key":数值, 返回数值字符串
    String extractVal(const String &json, const char *key) {
        String pat = String("\"") + key + "\":";
        int p = json.indexOf(pat);
        if (p < 0) return "";
        p += pat.length();
        // 跳过空格
        while (p < (int)json.length() && json[p] == ' ') p++;
        int start = p;
        while (p < (int)json.length() &&
               json[p] != ',' && json[p] != '}' && json[p] != ']') p++;
        return json.substring(start, p);
    }

    // 提取 bool (true/false)
    bool extractBool(const String &json, const char *key) {
        return extractVal(json, key) == "true";
    }

    // 解析 /api/status 的 JSON → TerminalState
    void parseStatus(const String &json) {
        TerminalState ns;
        // system 段
        ns.bz_mode = extractVal(json, "buzzer").toInt();
        ns.turn    = extractVal(json, "turn").toInt();
        ns.ind_l   = extractVal(json, "ind_left").toInt();
        ns.ind_r   = extractVal(json, "ind_right").toInt();
        ns.rcw_l   = extractBool(json, "rcw_l");
        ns.rcw_r   = extractBool(json, "rcw_r");
        // radar 段
        ns.obj_num = extractVal(json, "targets").toInt();
        ns.valid   = extractBool(json, "valid");
        ns.rx_bytes= extractVal(json, "bytes").toInt();
        ns.det_range = extractVal(json, "det_range").toInt();

        // 目标 list: 找 "range":N "angle":N "velo":N 模式 (按出现顺序)
        // list 里每个对象有 range/angle/velo, 我们按 "range" 关键词分割
        int m = min((int)ns.obj_num, 4);
        int searchFrom = 0;
        for (int i = 0; i < m; i++) {
            int pRange = json.indexOf("\"range\":", searchFrom);
            if (pRange < 0) break;
            int pAngle = json.indexOf("\"angle\":", pRange);
            int pVelo  = json.indexOf("\"velo\":", pAngle);
            if (pAngle < 0 || pVelo < 0) break;
            ns.objs[i].range    = json.substring(pRange + 8, json.indexOf(',', pRange)).toInt();
            ns.objs[i].angle    = json.substring(pAngle + 8, json.indexOf(',', pAngle)).toInt();
            ns.objs[i].velocity = json.substring(pVelo + 7, json.indexOf(',', pVelo)).toInt();
            ns.objs[i].id       = i;
            searchFrom = pVelo + 7;
        }

        state = ns;
        state.last_frame_ms = millis();
        state.online = true;
    }

public:
    TerminalState state;

    NetLink() : _last_poll(0), _wifi_connected(false) {}

    void init() {
        Serial.println("[NET] 连接主控 WiFi AP...");
        WiFi.mode(WIFI_STA);
        WiFi.begin(MASTER_SSID, MASTER_PASS);
        // 不阻塞等待, 在 update 里检测
    }

    // 主循环调用
    void update() {
        unsigned long now = millis();

        // WiFi 连接状态检测
        if (WiFi.status() != WL_CONNECTED) {
            if (_wifi_connected) {
                Serial.println("[NET] WiFi 断开, 重连...");
                _wifi_connected = false;
            }
            state.online = false;
            return;   // 没连上就不轮询
        }
        if (!_wifi_connected) {
            _wifi_connected = true;
            Serial.printf("[NET] 已连接 %s, IP=%s, RSSI=%d\n",
                          MASTER_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }

        // 周期轮询 /api/status
        if (now - _last_poll < POLL_INTERVAL_MS) return;
        _last_poll = now;

        _http.begin(_client, String(MASTER_HOST) + "/api/status");
        int code = _http.GET();
        if (code == 200) {
            String body = _http.getString();
            parseStatus(body);
        } else {
            Serial.printf("[NET] GET 失败 code=%d\n", code);
        }
        _http.end();
    }

    bool isWifiConnected() { return _wifi_connected; }

    // 触摸提交配置 (与主控 /api/config 的 POST 格式一致)
    // 主控 POST handler 收到后自动 saveToNVS + setBSDMode, 无需单独 SAVE
    bool sendConfig(const String &group, const String &key, int value) {
        if (!_wifi_connected) return false;
        String body = "{\"" + group + "\":{\"" + key + "\":" + String(value) + "}}";
        _http.begin(_client, String(MASTER_HOST) + "/api/config");
        _http.addHeader("Content-Type", "application/json");
        int code = _http.POST(body);
        _http.end();
        Serial.printf("[NET] POST %s = %d\n", body.c_str(), code);
        return code == 200;
    }

    // 兼容 config_view 的旧接口 (WiFi 模式下 POST 即保存, 无需单独操作)
    void sendSave()  { Serial.println("[NET] (保存已随 POST 完成)"); }
    void sendReset() { sendConfig("sys", "_reset", 1); /* 主控若无此 key 则忽略; 真正出厂重置见 /api/reset */ }

    // 出厂重置: GET/POST /api/reset
    bool sendFactoryReset() {
        if (!_wifi_connected) return false;
        _http.begin(_client, String(MASTER_HOST) + "/api/reset");
        int code = _http.POST("");
        _http.end();
        Serial.printf("[NET] factory reset = %d\n", code);
        return code == 200;
    }
};

extern NetLink netLink;
