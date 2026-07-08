// ============================================================
//  c3_terminal.ino - ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)
//  通信: UART1 (GPIO18/19) ← 主控 ESP32-32D UART1 (GPIO18/19 交叉)
//        $S 状态帧 10Hz 推送, $C 命令上行; 延迟 ~10ms
//  分阶段实现:
//    P1: UART 连接 + 收 $S 帧 + 串口打印     ✓
//    P2: 屏幕显示雷达扇形图                   ← 当前
//    P3: 触摸切页 + 参数配置
//    P4: ES8311 报警音同步
// ============================================================

#include <Arduino.h>
#include "lgfx_config.hpp"
#include "uart_link.h"

// 全局对象
LGFX lcd;
UartLink netLink;

// 分阶段开关 (开发时按需注释, 验证完逐个打开)
#define ENABLE_DISPLAY        // P1: 屏幕初始化
#define ENABLE_RADAR_VIEW     // P2: 雷达扇形图
// #define ENABLE_STATUS_VIEW    // P3: 状态页
// #define ENABLE_CONFIG_VIEW    // P3: 配置页 + 触摸
// #define ENABLE_ALERT_SOUND    // P4: ES8311 报警音

// 主控离线时显示模拟数据 (验证视图用; 接上主控后自动切换真实数据)
#define DEMO_WHEN_OFFLINE

#ifdef ENABLE_RADAR_VIEW
#include "radar_view.h"
RadarView radarView;
#endif

#ifdef ENABLE_STATUS_VIEW
#include "status_view.h"
StatusView statusView;
#endif

#ifdef ENABLE_CONFIG_VIEW
#include "config_view.h"
ConfigView configView;
#endif

#ifdef ENABLE_ALERT_SOUND
#include "alert_sound.h"
AlertSound alertSound;
#endif

// ============ 触摸/切页状态 ============
int currentPage = 0;        // 0=雷达图 1=状态 2=配置
int totalPages   = 1;        // 随分阶段开关增加
unsigned long lastTouchMs = 0;
int lastTouchX = -1, lastTouchY = -1;

void updatePages() {
    totalPages = 1;
#ifdef ENABLE_STATUS_VIEW
    totalPages++;
#endif
#ifdef ENABLE_CONFIG_VIEW
    totalPages++;
#endif
}

// ============ SETUP ============
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ebike-bsd C3 终端 V0.1 ===");
    Serial.println("Hardware: 立创实战派 ESP32-C3");

    // UART 链路 (接主控) — P1 阶段核心, 必须先于屏幕验证
    netLink.init();

    // 屏幕 (ENABLE_DISPLAY: 第1步验证屏幕点亮 + 颜色/方向)
#ifdef ENABLE_DISPLAY
    // ⚠ 背光: 实战派 C3 的背光 GPIO2 是低电平点亮 (官方 BK_LIGHT_ON_LEVEL=0)
    //   LovyanGFX 的 Light_PWM 默认高电平, 会反向. 先手动拉低点亮背光.
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW);   // 低电平点亮背光

    lcd.init();
    lcd.initDMA();

    Serial.printf("[LCD] %dx%d 初始化完成\n", lcd.width(), lcd.height());
#else
    Serial.println("[INIT] 屏幕未启用. 定义 ENABLE_DISPLAY 开启屏幕");
#endif

    // 主控离线时加载模拟数据 (验证雷达图视图)
#ifdef DEMO_WHEN_OFFLINE
    netLink.loadDemoData();
    Serial.println("[DEMO] 已加载模拟目标数据 (主控离线时显示)");
#endif

    updatePages();
    Serial.printf("[INIT] 页面数: %d\n", totalPages);
}

// ============ LOOP ============
void loop() {
    // 1. 读 UART, 更新 netLink.state
    netLink.update();

#ifdef ENABLE_DISPLAY
    // 2. 触摸处理 (切页 / 配置页调参)
    handleTouch();

    // 3. 按当前页绘制
    switch (currentPage) {
        case 0:
#ifdef ENABLE_RADAR_VIEW
            radarView.draw(netLink.state);
#endif
            break;
        case 1:
#ifdef ENABLE_STATUS_VIEW
            statusView.draw(netLink.state);
#endif
            break;
        case 2:
#ifdef ENABLE_CONFIG_VIEW
            configView.draw(netLink.state);
#endif
            break;
    }
#endif

    // 4. 报警音同步
#ifdef ENABLE_ALERT_SOUND
    alertSound.update(netLink.state.bz_mode);
#endif

    // 心跳 (每 2 秒, 确认固件在跑 + 显示在线状态)
    static unsigned long lastHb = 0;
    if (millis() - lastHb > 2000) {
        lastHb = millis();
        Serial.printf("[HB] online=%d last_frame=%lums ago\n",
                      netLink.state.online,
                      netLink.state.online ? (millis() - netLink.state.last_frame_ms) : 0);
    }

    delay(66);   // ~15fps 刷新 (主控 $S 推送 10Hz, 无需更快; 减少 SPI 总线占用防撕裂)
}

// ============ 触摸处理 ============
void handleTouch() {
    lcd.getTouch(nullptr);   // 触发触摸扫描 (LovyanGFX 内部)

    lgfx::touch_point_t tp;
    if (!lcd.getTouch(&tp)) {
        // 无触摸: 检测滑动结束 (简单实现: 不做滑动手势, 改用屏幕顶部/底部点击区切页)
        lastTouchX = -1;
        return;
    }

    // 防抖: 同一区域 300ms 内只响应一次
    if (millis() - lastTouchMs < 300) return;

    // 触摸区域划分:
    //   - 屏幕顶部 0~30px: 上一页
    //   - 屏幕底部 h-30~h: 下一页
    //   - 中间: 交给当前页处理 (config_view 的 <> 按钮区域)
    int h = lcd.height();
    bool handled = false;

    if (tp.y < 30) {
        // 上一页
        currentPage = (currentPage - 1 + totalPages) % totalPages;
        handled = true;
        Serial.printf("[TOUCH] 上一页 → %d\n", currentPage);
    } else if (tp.y > h - 30) {
        // 下一页
        currentPage = (currentPage + 1) % totalPages;
        handled = true;
        Serial.printf("[TOUCH] 下一页 → %d\n", currentPage);
    }
#ifdef ENABLE_CONFIG_VIEW
    else if (currentPage == 2) {
        handled = configView.handleTouch(tp.x, tp.y);
    }
#endif

    if (handled) lastTouchMs = millis();
}
