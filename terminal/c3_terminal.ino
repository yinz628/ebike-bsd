// ============================================================
//  c3_terminal.ino - ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)
//  功能: 通过 UART 接收主控状态 → ST7789 显示 + ES8311 报警音
//  分阶段实现:
//    P1: UART 接收 + 串口打印           (先验证通信)
//    P2: 屏幕显示雷达扇形图              (本文件含 radar_view)
//    P3: 触摸切页 + 参数配置             (status_view/config_view)
//    P4: ES8311 报警音同步               (alert_sound)
// ============================================================

#include <Arduino.h>
#include "lgfx_config.hpp"
#include "uart_link.h"

// 全局对象
LGFX lcd;
UartLink link;

// 分阶段开关 (开发时按需注释, 验证完逐个打开)
#define ENABLE_RADAR_VIEW     // P2: 雷达扇形图
// #define ENABLE_STATUS_VIEW    // P3: 状态页
// #define ENABLE_CONFIG_VIEW    // P3: 配置页 + 触摸
// #define ENABLE_ALERT_SOUND    // P4: ES8311 报警音

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

    // UART 链路 (接主控)
    link.init();

    // 屏幕
    lcd.init();
    lcd.initDMA();
    lcd.setBrightness(180);     // 0-255, 适中亮度
    lcd.fillScreen(lgfx::color888(13, 17, 23));
    lcd.setTextColor(lgfx::color888(88, 166, 255));
    lcd.setTextSize(2);
    lcd.setCursor(40, 100);
    lcd.print("eBike-BSD");
    lcd.setTextSize(1);
    lcd.setCursor(60, 130);
    lcd.print("C3 Terminal booting...");
    lcd.display();

    updatePages();
    Serial.printf("[INIT] 页面数: %d\n", totalPages);
}

// ============ LOOP ============
void loop() {
    // 1. 读 UART, 更新 link.state
    link.update();

    // 2. 触摸处理 (切页 / 配置页调参)
    handleTouch();

    // 3. 按当前页绘制
    switch (currentPage) {
        case 0:
#ifdef ENABLE_RADAR_VIEW
            radarView.draw(link.state);
#endif
            break;
        case 1:
#ifdef ENABLE_STATUS_VIEW
            statusView.draw(link.state);
#endif
            break;
        case 2:
#ifdef ENABLE_CONFIG_VIEW
            configView.draw(link.state);
#endif
            break;
    }

    // 4. 报警音同步
#ifdef ENABLE_ALERT_SOUND
    alertSound.update(link.state.bz_mode);
#endif

    delay(20);   // ~50fps 刷新
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
