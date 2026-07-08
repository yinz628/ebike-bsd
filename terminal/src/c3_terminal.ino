// ============================================================
//  c3_terminal.ino - ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)
//  通信: UART1 (GPIO18/19) ← 主控 ESP32-32D UART1 (GPIO21/22)
//        $S 状态帧 10Hz 推送, $C 命令上行; 延迟 ~10ms
//  分阶段实现:
//    P1: UART 连接 + 收 $S 帧 + 串口打印
//    P2: 屏幕显示雷达扇形图
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
#define ENABLE_DISPLAY        // P1: 屏幕初始化 (静态测试画面)
// #define ENABLE_RADAR_VIEW     // P2: 雷达扇形图
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

    // 静态测试画面: 验证颜色、方向、分辨率
    lcd.fillScreen(lgfx::color888(13, 17, 23));      // 深蓝黑背景

    // 标题 (蓝色)
    lcd.setTextColor(lgfx::color888(88, 166, 255));
    lcd.setTextSize(2);
    lcd.setCursor(20, 10);
    lcd.print("eBike-BSD C3");

    // 分辨率标注 (横屏应为 320x240)
    lcd.setTextSize(1);
    lcd.setTextColor(lgfx::color888(139, 148, 158));
    lcd.setCursor(20, 32);
    lcd.printf("%dx%d", lcd.width(), lcd.height());

    // 三色条 (验证红蓝不反): 红 黄 绿
    lcd.fillRect(20, 50, 80, 30, lgfx::color888(248, 81, 73));    // 红
    lcd.fillRect(120, 50, 80, 30, lgfx::color888(210, 153, 34));  // 黄
    lcd.fillRect(220, 50, 80, 30, lgfx::color888(63, 185, 80));   // 绿
    lcd.setTextColor(lgfx::color888(255, 255, 255));
    lcd.setCursor(40, 60); lcd.print("R");
    lcd.setCursor(155, 60); lcd.print("Y");
    lcd.setCursor(255, 60); lcd.print("G");

    // 四角标记 (验证分辨率边界 + 方向)
    lcd.setTextColor(lgfx::color888(201, 209, 217));
    lcd.setCursor(2, 2); lcd.print("TL");           // 左上
    lcd.setCursor(lcd.width()-22, 2); lcd.print("TR");  // 右上
    lcd.setCursor(2, lcd.height()-12); lcd.print("BL"); // 左下
    lcd.setCursor(lcd.width()-22, lcd.height()-12); lcd.print("BR"); // 右下

    // 中间圆 + 三角 (验证几何绘制)
    int cx = lcd.width()/2, cy = 150;
    lcd.drawCircle(cx, cy, 25, lgfx::color888(88, 166, 255));
    lcd.fillTriangle(cx-20, cy+40, cx+20, cy+40, cx, cy+10, lgfx::color888(210, 153, 34));

    // 底部状态
    lcd.setTextColor(lgfx::color888(63, 185, 80));
    lcd.setCursor(20, lcd.height()-20);
    lcd.print("Screen OK - touch/sound TBD");

    Serial.println("[LCD] 测试画面已绘制 (三色条+四角标记+几何)");
#else
    Serial.println("[INIT] 屏幕未启用. 定义 ENABLE_DISPLAY 开启屏幕");
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
