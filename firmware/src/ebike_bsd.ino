// ============================================================
//  ebike_bsd.ino - 电动自行车进阶转向灯系统 主程序
//  硬件: ESP32 + MS60-3015 60GHz雷达 ×1 + IRLZ44N MOSFET ×4
//  版本: V2.7 — WiFi控制台 + JSON配置 + LED闪烁分级 + 目标自动消失 + StaticJsonDocument修复
// ============================================================

#include <Arduino.h>
#include "bsd_protocol.h"
#include "ms60_radar.h"
#include "led_control.h"
#include "config_store.h"
#include <esp_task_wdt.h>
#include <esp_bt.h>        // btStop() 关闭蓝牙控制器 (主控仅用 WiFi AP, 不用蓝牙)

#define WDT_TIMEOUT_S 5

// ============ 引脚定义 ============
#define SERIAL_DEBUG_BAUD  115200

// UART2: 单雷达 (ESP32 UART2: GPIO16=RX, GPIO17=TX)
#define RADAR_UART_NUM     2
#define RADAR_RX_PIN       16
#define RADAR_TX_PIN       17
#define RADAR_BAUD         921600

// MOSFET 栅极 (同侧前后灯共用一个GPIO)
#define LED_LEFT_PIN       4    // 左前+左后
#define LED_RIGHT_PIN      23   // 右前+右后 (原GPIO5, 启动引脚冲突)

// 开关输入 (内部上拉, LOW=触发)
#define SW_LEFT_PIN        13   // 左转向
#define SW_RIGHT_PIN       14   // 右转向

// 蜂鸣器 (12V高分贝, IRLZ44N MOSFET驱动, HIGH=响)
#define BUZZER_PIN         12

// 指示灯 (GPIO直驱, 不需要MOSFET, 串联220Ω限流即可)
#define INDICATOR_L_PIN    25   // 左后碰撞指示灯
#define INDICATOR_R_PIN    26   // 右后碰撞指示灯

// 注: 原 OLED I2C 定义 (GPIO21 SDA / GPIO22 SCL) 已删除
// 主控不接 OLED, 这两个脚现在给 UART1 (terminal_link.h) 用作 TX/RX 连接车把 C3 终端

// ============ 常量 ============
#define BLINK_INTERVAL_NORMAL   667   // 1.5Hz 正常闪烁 (ms)
#define BSD_CRITICAL_SPEED     2      // m/s, >7.2km/h 视为接近
#define BSD_CRITICAL_RANGE     30     // 米, 30米内视为危险
#define MAIN_LOOP_DELAY        50     // 主循环周期 (ms)
#define BUZZER_BEEP_DURATION   150    // 蜂鸣单次时长 (ms)
#define STATE_DEBOUNCE_MS      50     // 按键消抖


// LED 指示灯模式 (独立状态机)
#define IND_MODE_OFF    0  // 灭
#define IND_MODE_BSD    1  // 1Hz 慢闪 (盲区)
#define IND_MODE_RCW    2  // 4Hz 快闪 (碰撞预警)
#define IND_MODE_TURN   3  // 常亮 (转向辅助)

// 闪烁定时: 使用 (millis()/interval) % 2 方式
// 所有 LED 都通过 updateIndicatorLEDs() 集中控制

// ============ 全局对象 ============
MS60Radar radar(RADAR_UART_NUM, RADAR_RX_PIN, RADAR_TX_PIN, RADAR_BAUD);
LEDController ledCtrl;
ConfigStore config;  // 全局配置实例

// ============ 状态变量 ============
typedef enum {
    TURN_OFF = 0,
    TURN_LEFT,
    TURN_RIGHT
} TurnState_t;

TurnState_t turn_state = TURN_OFF;
#include "wifi_web.h"
#include "terminal_link.h"   // 车把 C3 终端 UART1 链路 (新增, 独立于 WiFi)

unsigned long last_blink_time = 0;
unsigned long last_bsd_beep_time = 0;
unsigned long last_buzzer_change = 0;
bool turn_led_on = false;         // 当前LED亮灭
bool bsd_l_warned = false;       // 左侧已预警
bool bsd_r_warned = false;       // 右侧已预警
bool bsd_l_active = false;       // 左盲区当前有目标
bool bsd_r_active = false;       // 右盲区当前有目标
unsigned long bsd_l_led_time = 0;  // 左LED最后触发时间
unsigned long bsd_r_led_time = 0;  // 右LED最后触发时间

// LED 模式 (由各子系统设置, updateIndicatorLEDs 集中刷新)
int ind_left_mode  = IND_MODE_OFF;   // 左指示灯当前模式
int ind_right_mode = IND_MODE_OFF;   // 右指示灯当前模式
// 闪烁用常量 (初始化默认值, 运行时从config读取)
#define BSD_IND_DEFAULT    500   // 1Hz 慢闪
#define RCW_IND_DEFAULT    125   // 4Hz 快闪

int buzzer_mode = 0;              // 0=静音, 1=短鸣(BSD), 2=4Hz(RCW), 3=长鸣(转向辅助)
unsigned long alarm_test_until = 0;  // ALARM 测试持续到此时间 (0=非测试)

// WiFi AP 运行状态 (全局, 供 terminal_link.h 的 $WIFI 命令读写)
bool g_wifi_running = true;
unsigned long g_wifi_idle_since = 0;   // AP 空闲计时起点 (0=有连接或未开始计时)

// RCW 状态 (参数见config.rcw)
bool rcw_l_active = false;
bool rcw_r_active = false;
unsigned long rcw_l_time = 0;
unsigned long rcw_r_time = 0;

// ============ 函数声明 ============
void setupPins();
void updateTurnControl();
void updateTurnAssist();
void updateRCW();
void updateBuzzer();
void blinkLEDs();
void setTurnLEDs(bool on, bool left, bool right);
void updateIndicatorLEDs();   // 集中LED状态机
void debugOutput();

// ===============================================================
//  SETUP
// ===============================================================
uint8_t hexDigit(char c) { if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; if(c>='a'&&c<='f')return c-'a'+10; return 0; }

void setup() {
    // 禁用任务看门狗 (setup耗时较长: WiFi+雷达初始化+Flash保存)
    // 禁用任务看门狗 (setup耗时较长)
    esp_task_wdt_deinit();
    
    Serial.begin(SERIAL_DEBUG_BAUD);
    Serial.println();
    Serial.println("=== e-Bike BSD Turn Signal System (V2.7) ===");
    Serial.println("Hardware: ESP32 + MS60-3015 x1 (居中安装)");

    // 加载配置
    // ⚠ loadFromNVS 失败时不写 NVS: 失败原因可能是 NVS 分区被破坏(otadata 越界擦除等),
    // 此时若 saveToNVS(默认值) 会覆盖用户配置 → 每次重启配置丢失.
    // 首次烧录无配置时, 结构体的默认值会自然生效, 等用户主动改配置时才写 NVS.
    Serial.println("[CONFIG] 加载...");
    if (!config.loadFromNVS()) {
        Serial.println("[CONFIG] NVS 无配置或读取失败, 使用默认值 (不写 NVS)");
    }
    config.summary();

    // WiFi AP + Web控制台 (每次开机都启动, 30秒无人连自动关)
    // 关闭蓝牙控制器 (主控仅用 WiFi AP 调试, 不用蓝牙, 省电)
    btStop();
    Serial.println("[BT] 蓝牙控制器已关闭");

    Serial.println("[WIFI] 启动热点 (30秒无人连自动关)...");
    initWiFi();
    initWebServer();

    setupPins();

    // 初始化 LED 控制器
    ledCtrl.begin(LED_LEFT_PIN, LED_RIGHT_PIN);

    // 初始化单雷达 (UART2)
    Serial.println("[RADAR] 初始化雷达 (UART2, GPIO16/17, 921600bps)...");
    if (radar.begin()) {
        Serial.println("[RADAR] 雷达 UART OK, 等待模块启动...");
        for (int i = 0; i < 30; i++) { delay(100); yield(); }  // 3秒分段, 防看门狗
        radar.setBSDMode();
        Serial.println("[RADAR] BSD模式已设置");
    } else {
        Serial.println("[RADAR] ⚠ 雷达初始化失败, 请检查接线");
    }

    // 重新初始化看门狗 (5秒超时, 仅loopTask喂狗)
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);
    
    Serial.println("=== 系统就绪 ===");
    Serial.println("[WEB] 手机连接 eBike-BSD 热点, 浏览器打开 192.168.4.1");

    // C3 车把终端 UART 链路 (可选, 无 C3 时静默)
    terminalLinkInit();
}

// ===============================================================
//  LOOP
// ===============================================================
void loop() {
    // ==== WiFi 自动关状态机 ====
    // 规则:
    //   - 开机 / C3 手动开 / Web 触发: WiFi 开
    //   - 任何来源开启后, 无设备连接 → 30s 后关 WiFi
    //   - 有设备连接 → 保持 WiFi 开
    //   - 设备断开 → 重新 30s 倒计时 → 关 WiFi
    static unsigned long wifi_check = 0;

    if (g_wifi_running && millis() - wifi_check > 2000) {
        wifi_check = millis();

        int sta_num = WiFi.softAPgetStationNum();

        Serial.print("[WIFI] sta=");
        Serial.print(sta_num);
        Serial.print(" idle=");
        Serial.print(g_wifi_idle_since ? (millis() - g_wifi_idle_since) / 1000 : 0);
        Serial.println("s");

        if (sta_num > 0) {
            // 有设备连接 → 重置计时
            g_wifi_idle_since = 0;
        } else {
            // 无设备连接
            if (g_wifi_idle_since == 0) {
                g_wifi_idle_since = millis();   // 开始空闲计时
            }

            if (millis() - g_wifi_idle_since >= 30000) {
                Serial.println("[WIFI] 30s无连接, 关闭WiFi");
                server.end();
                delay(50);
                WiFi.softAPdisconnect(true);
                WiFi.enableAP(false);
                WiFi.mode(WIFI_OFF);
                g_wifi_running = false;
            }
        }
    }

    // 1) 读取开关（含消抖）
    updateTurnControl();

    // 2) 读取雷达数据 (单雷达)
    radar.readFrame();
    radar.checkStale(500);  // 500ms无新帧→清零, 防止旧目标残留
    
    // 2.5) 自动检测雷达重连并重新配置
    static bool radar_configured = false;
    static unsigned long last_radar_rx = 0;
    if (radar.getTotalBytes() > 0) last_radar_rx = millis();
    // 雷达有数据 (>50字节 - 确保boot日志完全输出) 且尚未配置 → 发配置
    if (!radar_configured && radar.getTotalBytes() > 50) {
        radar_configured = true;
        Serial.println("[AUTO-CFG] 雷达启动完成，发送配置...");
        delay(500);  // 等雷达完全就绪
        radar.setBSDMode();
    }
    // 超时 5 秒无数据 → 重置检测 (雷达可能被热插拔了)
    if (radar_configured && (millis() - last_radar_rx) > 5000 && radar.getTotalBytes() == 0) {
        radar_configured = false;
        Serial.println("[AUTO-CFG] 雷达静默，等待重连...");
    }
    // 首次运行 5 秒后仍未检测到 → 主动发送配置
    static bool proactive_sent = false;
    if (!radar_configured && !proactive_sent && millis() > 5000) {
        proactive_sent = true;
        Serial.println("[AUTO-CFG] 超时主动配置...");
        delay(100);
        radar.setBSDMode();
        radar_configured = true;
    }

    // 2.6) 串口命令转发 (PC→ESP32→雷达)
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        
        if (line == "BEEP") {
            digitalWrite(BUZZER_PIN, HIGH); delay(200);
            digitalWrite(BUZZER_PIN, LOW);
            Serial.println("[FWD] BEEP test");
        }
        else if (line == "ALARM") {
            // 触发 RCW 蜂鸣 3 秒 (测试主控蜂鸣器 + C3 扬声器同步)
            alarm_test_until = millis() + 3000;
            Serial.println("[FWD] ALARM test (RCW 4Hz, 3s)");
        }
        else if (line == "SAVE") {
            config.saveToNVS();
            Serial.println("[FWD] Config saved to NVS");
        }
        else if (line == "RESET") {
            config.factoryReset();
            radar.setBSDMode();  // 将出厂配置立即下发给雷达 (灵敏度/距离)
            Serial.println("[FWD] Factory reset done, NVS cleared, radar reconfigured");
        }
        else if (line == "DUMP") {
            static StaticJsonDocument<4096> dump_doc;
            dump_doc.clear();
            config.toJson(dump_doc);
            serializeJson(dump_doc, Serial);
            Serial.println();
        }
        else if (line == "LOAD") {
            if (!config.loadFromNVS()) {
                Serial.println("[CONFIG] NVS 无配置或读取失败, 使用默认值 (不写 NVS)");
            }
            config.summary();
        }
        else if (line.startsWith("CMD:")) {
            // 解析hex: CMD:58D102005B00
            String hex = line.substring(4);
            uint8_t buf[32]; int len = 0;
            for (int i = 0; i < (int)hex.length() && len < 32; i += 2) {
                if (i+1 < (int)hex.length()) {
                    char c1 = hex[i], c2 = hex[i+1];
                    if (isxdigit(c1) && isxdigit(c2)) {
                        buf[len++] = (hexDigit(c1) << 4) | hexDigit(c2);
                    }
                }
            }
            if (len > 0) {
                radar.sendCmd(buf, len);  // 通过雷达对象发送 (封装的 UART)
                Serial.print("[FWD] sent "); Serial.print(len); Serial.println(" bytes");
            }
        }
    }
    
    
    // 3) 后方监测 (BSD+RCW合并): 低速→慢闪, 高速→快闪
    ind_left_mode = IND_MODE_OFF;
    ind_right_mode = IND_MODE_OFF;
    updateRCW();

    // 4) 转向辅助 (打转向灯时判断该方向是否有危险)
    if (turn_state == TURN_LEFT || turn_state == TURN_RIGHT) {
        updateTurnAssist();
    }

    // 6) LED 闪烁控制 (转向灯 PWM)
    blinkLEDs();

    // 6.5) 指示灯状态机 (BSD/RCW/转向辅集中刷新)
    updateIndicatorLEDs();

    // 7) 蜂鸣器状态机
    updateBuzzer();

    // 7.5) 车把 C3 终端链路 (推送状态 + 接收触摸命令)
    terminalLinkUpdate();

    // 8) 调试输出 (每2秒, 仅在编译开关 ENABLE_DEBUG_LOG 开启时输出)
    //    日常运行关闭以减少串口/USB 开销; 调试时在 platformio.ini 定义该宏
#ifdef ENABLE_DEBUG_LOG
    static unsigned long last_debug = 0;
    if (millis() - last_debug > 2000) {
        last_debug = millis();
        debugOutput();
    }
#endif

    esp_task_wdt_reset();    // 喂看门狗

    delay(MAIN_LOOP_DELAY);
    yield();                 // 让出CPU给WiFi/Web回调
}

// ===============================================================
//  引脚初始化
// ===============================================================
void setupPins() {
    // MOSFET 栅极 (初始LOW=关闭)
    pinMode(LED_LEFT_PIN, OUTPUT);
    pinMode(LED_RIGHT_PIN, OUTPUT);
    digitalWrite(LED_LEFT_PIN, LOW);
    digitalWrite(LED_RIGHT_PIN, LOW);

    // 开关 (内部上拉)
    pinMode(SW_LEFT_PIN, INPUT_PULLUP);
    pinMode(SW_RIGHT_PIN, INPUT_PULLUP);

    // 蜂鸣器
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    // === 蜂鸣器自检 (短鸣 3 声) ===
    Serial.println("[TEST] 蜂鸣器自检...");
    for(int i=0;i<3;i++){
        digitalWrite(BUZZER_PIN, HIGH); delay(100);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }

    // 指示灯 (GPIO直驱LED, 串联220Ω, 初始灭)
    pinMode(INDICATOR_L_PIN, OUTPUT);
    pinMode(INDICATOR_R_PIN, OUTPUT);
    digitalWrite(INDICATOR_L_PIN, LOW);
    digitalWrite(INDICATOR_R_PIN, LOW);
}

// ===============================================================
//  转向控制 — 自锁式三档开关 (LEFT=LOW / RIGHT=LOW / 中间=OFF)
// ===============================================================
void updateTurnControl() {
    bool sw_l = digitalRead(SW_LEFT_PIN);
    bool sw_r = digitalRead(SW_RIGHT_PIN);

    // 三档开关 (自锁式, 直接读电平)
    if (sw_l == LOW && turn_state != TURN_LEFT) {
        turn_state = TURN_LEFT;
        Serial.println("[SW] 左转向 ON");
    } else if (sw_r == LOW && turn_state != TURN_RIGHT) {
        turn_state = TURN_RIGHT;
        Serial.println("[SW] 右转向 ON");
    } else if (sw_l == HIGH && sw_r == HIGH && turn_state != TURN_OFF) {
        turn_state = TURN_OFF;
        Serial.println("[SW] 转向 OFF");
    }
}

// ===============================================================
//  盲区监测 (BSD) — 单雷达, angle判左右
// ===============================================================


// ===============================================================
//  转向辅助: 打灯时判断同侧危险
//  从同一个雷达数据中, 按 angle 筛选同侧目标
// ===============================================================
void updateTurnAssist() {
    BSDFrame *f = radar.getFrame();
    if (!f->valid) return;   // 帧无效则跳过

    // 帧新鲜度检查: 只用新帧, 避免旧帧重复触发
    static uint32_t last_ts_turn = 0;
    if (f->timestamp == last_ts_turn) return;
    last_ts_turn = f->timestamp;

    bool danger = false;

    for (int i = 0; i < f->obj_num && i < 8; i++) {
        BSDObj *obj = &f->objects[i];
        if (obj->range <= 0 || obj->range >= config.turn.range_limit) continue;
        if (obj->velocity < config.turn.speed_threshold) continue;

        // 横向距离过滤 (同 RCW): lateral = range × sin(|angle|)
        float latRad = abs(obj->angle) * PI / 180.0f;
        int lateral = (int)(obj->range * sinf(latRad));
        if (lateral > config.turn.lateral_limit) continue;

        if (turn_state == TURN_LEFT) {
            // 左转: 角度在左后方 -40°~-5° 且有接近速度
            if (TURN_ANGLE_IS_LEFT(obj->angle)) {
                danger = true;
                break;
            }
        } else if (turn_state == TURN_RIGHT) {
            // 右转: 角度在右后方 +5°~+40°
            if (TURN_ANGLE_IS_RIGHT(obj->angle)) {
                danger = true;
                break;
            }
        }
    }

    if (danger) {
        // 驱动同侧指示灯 (GPIO直驱LED) 作为视觉警示
        if (turn_state == TURN_LEFT) {
            ind_left_mode = IND_MODE_TURN;
        } else {
            ind_right_mode = IND_MODE_TURN;
        }
        buzzer_mode = 3;  // 转向辅助: 持续长鸣 (不受蜂鸣开关控制)
        Serial.println("[ASSIST] ⚠ 转向侧有接近车辆！");
    } else {
        // 仅清除本侧指示灯, 不覆盖 RCW 已设的
        if (turn_state == TURN_LEFT && !rcw_l_active) {
            // LED mode cleared by ind_left_mode reset at top of loop
        } else if (turn_state == TURN_RIGHT && !rcw_r_active) {
            // LED mode cleared by ind_right_mode reset at top of loop
        }
        if ((buzzer_mode == 2 || buzzer_mode == 3) && !rcw_l_active && !rcw_r_active) {
            buzzer_mode = 0;
        }
    }
}

// ===============================================================
//  后碰撞预警 (RCW)
//  无转向灯时, 检测后方快速接近 → 亮对应指示灯
//  指示灯(5mm LED, GPIO直驱) 常亮, 蜂鸣器4Hz响
//  与转向灯完全独立, 互不干扰
// ===============================================================
// ===============================================================
//  后方监测 (BSD+RCW合并): 按速度两级报警
//    低速 (≥low_speed): IND_MODE_BSD (1Hz慢闪) + 蜂鸣单声
//    高速 (≥speed_threshold): IND_MODE_RCW (4Hz快闪) + 蜂鸣4Hz
// ===============================================================
void updateRCW() {
    // ALARM 测试期间: 强制 RCW 蜂鸣 (绕过正常检测, 用于测试蜂鸣同步)
    if (alarm_test_until) {
        if (millis() < alarm_test_until) {
            buzzer_mode = config.sys.rcw_buzzer ? 2 : 0;  // 受蜂鸣开关控制
            return;
        }
        alarm_test_until = 0;
        buzzer_mode = 0;
    }

    BSDFrame *f = radar.getFrame();
    if (!f->valid) {
        buzzer_mode = 0;
        return;
    }
    unsigned long now = millis();
    
    // 新帧→更新检测状态
    static uint32_t last_ts = 0;
    static unsigned long last_low_beep_l = 0;
    static unsigned long last_low_beep_r = 0;
    if (f->timestamp != last_ts) {
        last_ts = f->timestamp;
        
        bool leftLow=false, rightLow=false, leftHigh=false, rightHigh=false;
        for (int i = 0; i < f->obj_num && i < 8; i++) {
            BSDObj *obj = &f->objects[i];
            if (obj->range <= 0 || obj->range > config.rcw.range_limit) continue;

            // 横向距离过滤: lateral = range × sin(|angle|)
            // 横向距离远的车辆 (如 ±40° 边缘的远处车) 不会构成后方碰撞, 跳过
            // 避免"探测范围内横向很远的汽车被误报"
            float latRad = abs(obj->angle) * PI / 180.0f;
            int lateral = (int)(obj->range * sinf(latRad));
            if (lateral > config.rcw.lateral_limit) continue;

            // 速度分级
            bool isHigh = (obj->velocity >= config.rcw.speed_threshold);
            bool isLow  = (obj->velocity >= config.rcw.low_speed && !isHigh);
            
            if (REAR_ANGLE_IS_LEFT(obj->angle)) {
                if (isHigh) leftHigh = true;
                else if (isLow) leftLow = true;
            }
            if (REAR_ANGLE_IS_RIGHT(obj->angle)) {
                if (isHigh) rightHigh = true;
                else if (isLow) rightLow = true;
            }
        }
        
        // 左侧: 高优先 → 低优先 (蜂鸣受 rcw_buzzer 开关控制)
        if (leftHigh) {
            if (!rcw_l_active) Serial.println("[RCW] ⚡ 左后方有车辆快速接近!");
            rcw_l_active = true; rcw_l_time = now;
            ind_left_mode = IND_MODE_RCW;
            if (config.sys.rcw_buzzer) buzzer_mode = 2;
        } else if (leftLow) {
            if (!rcw_l_active) Serial.println("[RCW] 🔹 左侧盲区有车辆");
            rcw_l_active = true; rcw_l_time = now;
            ind_left_mode = IND_MODE_BSD;
            // 低速蜂鸣有5秒冷却
            if (config.sys.rcw_buzzer && now - last_low_beep_l > (unsigned long)config.sys.bsd_beep_cooldown) {
                buzzer_mode = 1; last_low_beep_l = now;
            }
        }
        // 右侧: 高优先 → 低优先
        if (rightHigh) {
            if (!rcw_r_active) Serial.println("[RCW] ⚡ 右后方有车辆快速接近!");
            rcw_r_active = true; rcw_r_time = now;
            ind_right_mode = IND_MODE_RCW;
            if (config.sys.rcw_buzzer) buzzer_mode = 2;
        } else if (rightLow) {
            if (!rcw_r_active) Serial.println("[RCW] 🔹 右侧盲区有车辆");
            rcw_r_active = true; rcw_r_time = now;
            ind_right_mode = IND_MODE_BSD;
            if (config.sys.rcw_buzzer && now - last_low_beep_r > (unsigned long)config.sys.bsd_beep_cooldown) {
                buzzer_mode = 1; last_low_beep_r = now;
            }
        }
    }
    
    // 超时解除
    if (rcw_l_active && (now - rcw_l_time > (unsigned long)config.rcw.hold_time)) {
        rcw_l_active = false;
    }
    if (rcw_r_active && (now - rcw_r_time > (unsigned long)config.rcw.hold_time)) {
        rcw_r_active = false;
    }
    if (!rcw_l_active && !rcw_r_active) buzzer_mode = 0;
}

// ===============================================================
//  LED 闪烁控制
// ===============================================================
void blinkLEDs() {
    unsigned long now = millis();
    unsigned long interval;


    if (now - last_blink_time >= BLINK_INTERVAL_NORMAL) {
        last_blink_time = now;
        turn_led_on = !turn_led_on;

        bool isLeft = (turn_state == TURN_LEFT);
        bool isRight = (turn_state == TURN_RIGHT);

        setTurnLEDs(turn_led_on, isLeft, isRight);
    }
}

// ===============================================================
//  LED 通道控制
// ===============================================================
void setTurnLEDs(bool on, bool left, bool right) {
    ledCtrl.set(LED_LEFT_PIN,  left  ? on : false);
    ledCtrl.set(LED_RIGHT_PIN, right ? on : false);
}

// ===============================================================
//  指示灯状态机 (BSD/RCW/转向辅 集中刷新)
//  模式: IND_MODE_OFF=灭, BSD=1Hz慢闪, RCW=4Hz快闪, TURN=常亮
// ===============================================================
void updateIndicatorLEDs() {
    unsigned long now = millis();
    
    // 左指示灯
    switch (ind_left_mode) {
        case IND_MODE_OFF:
            digitalWrite(INDICATOR_L_PIN, LOW);
            break;
        case IND_MODE_BSD:
            digitalWrite(INDICATOR_L_PIN, ((now / (unsigned long)config.rcw.lflash_interval) % 2 == 0));
            break;
        case IND_MODE_RCW:
            digitalWrite(INDICATOR_L_PIN, ((now / (unsigned long)config.rcw.flash_interval) % 2 == 0));
            break;
        case IND_MODE_TURN:
            digitalWrite(INDICATOR_L_PIN, HIGH);
            break;
    }
    
    // 右指示灯
    switch (ind_right_mode) {
        case IND_MODE_OFF:
            digitalWrite(INDICATOR_R_PIN, LOW);
            break;
        case IND_MODE_BSD:
            digitalWrite(INDICATOR_R_PIN, ((now / (unsigned long)config.rcw.lflash_interval) % 2 == 0));
            break;
        case IND_MODE_RCW:
            digitalWrite(INDICATOR_R_PIN, ((now / (unsigned long)config.rcw.flash_interval) % 2 == 0));
            break;
        case IND_MODE_TURN:
            digitalWrite(INDICATOR_R_PIN, HIGH);
            break;
    }
}

// ===============================================================
//  蜂鸣器状态机
// ===============================================================
void updateBuzzer() {
    unsigned long now = millis();

    switch (buzzer_mode) {
        case 0:  // 静音
            digitalWrite(BUZZER_PIN, LOW);
            break;

        case 1:  // 单次短鸣
            if (now - last_buzzer_change < BUZZER_BEEP_DURATION) {
                digitalWrite(BUZZER_PIN, HIGH);
            } else {
                digitalWrite(BUZZER_PIN, LOW);
                buzzer_mode = 0;
            }
            break;

        case 2:  // RCW: 4Hz beep (仅碰撞预警)
            if (rcw_l_active || rcw_r_active) {
                int beepInterval = config.rcw.flash_interval;
                bool beepOn = ((now / beepInterval) % 2 == 0);
                digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
            } else {
                digitalWrite(BUZZER_PIN, LOW);
                buzzer_mode = 0;
            }
            break;

        case 3:  // 转向辅助: 持续长鸣
            digitalWrite(BUZZER_PIN, HIGH);
            break;
    }
}

// ===============================================================
//  调试输出 (串口)
// ===============================================================
void debugOutput() {
    static int counter = 0;
    counter++;

    BSDFrame *f = radar.getFrame();

    Serial.print("=== [");
    Serial.print(counter);
    Serial.print("] RX:");
    Serial.print(radar.getTotalBytes());
    Serial.print(" TURN:");
    switch (turn_state) {
        case TURN_OFF:    Serial.print("OFF"); break;
        case TURN_LEFT:   Serial.print("L"); break;
        case TURN_RIGHT:  Serial.print("R"); break;
    }
    // RCW 状态
    if (rcw_l_active) Serial.print(" RCW-L");
    if (rcw_r_active) Serial.print(" RCW-R");
    // 首次收到字节时 dump raw hex log
    if (radar.getRawLogLen() > 0) {
        Serial.print(" RAW=");
        int n = radar.getRawLogLen();
        const uint8_t *log = radar.getRawLog();
        for (int i = 0; i < n && i < 48; i++) {
            if (log[i] < 0x10) Serial.print("0");
            Serial.print(log[i], HEX);
            Serial.print(" ");
        }
    }

    Serial.print(" | BSD:");
    Serial.print(f->obj_num);
    Serial.print("tgt");
    if (f->valid) Serial.print(" V"); else Serial.print(" !");
    if (bsd_l_active) Serial.print(" L*");
    if (bsd_r_active) Serial.print(" R*");

    for (int i = 0; i < f->obj_num && i < 4; i++) {
        Serial.print(" [");
        Serial.print(f->objects[i].range);
        Serial.print("m,");
        Serial.print(f->objects[i].angle);
        Serial.print("°,");
        Serial.print(f->objects[i].velocity);
        Serial.print("m/s,ID");
        Serial.print(f->objects[i].objId);
        Serial.print("]");
    }

    Serial.print(" | BZ:");
    Serial.print(buzzer_mode);
    Serial.print(" | TURN:");
    Serial.print(turn_led_on ? "ON" : "OFF");
    Serial.print(" | IND:");
    bool ind_l = digitalRead(INDICATOR_L_PIN);
    bool ind_r = digitalRead(INDICATOR_R_PIN);
    if (ind_l) Serial.print("L");
    if (ind_r) Serial.print("R");
    if (!ind_l && !ind_r) Serial.print("-");

    // 角度方向文字标识
    if (f->obj_num > 0) {
        Serial.print(" | DIR:");
        for (int i = 0; i < f->obj_num && i < 3; i++) {
            int8_t a = f->objects[i].angle;
            if (ANGLE_IS_LEFT(a))       Serial.print("L");
            else if (ANGLE_IS_RIGHT(a)) Serial.print("R");
            else if (ANGLE_IS_CENTER(a)) Serial.print("C");
            else                        Serial.print("?");
        }
    }

    Serial.println();
}
