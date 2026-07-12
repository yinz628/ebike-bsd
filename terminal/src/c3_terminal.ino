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
#include <Wire.h>
#include "lgfx_config.hpp"
#include "uart_link.h"

// ============ 固件版本 (单一真源) ============
// platformio.ini -D APP_VERSION 同步写入 esp_app_desc_t, 供 OTA 比对.
#ifndef FW_VERSION
#define FW_VERSION "V0.2"
#endif

// 全局对象 (uart_link.h 里只 extern 声明, 实际定义在此)
LGFX lcd;
UartLink netLink;
C3OtaProgress c3OtaProgress;

// ============ FT6336 触摸裸 I2C 读取 ============
// 不用 LovyanGFX 内置驱动 (横屏旋转坐标有偏移), 直接读 FT6336 寄存器 + 手动变换
//
// 实测原始坐标范围 (本屏, offset_rotation=1):
//   raw_x ∈ [25, 212]   (物理上下方向, 上=212 下=25, 跨度187)
//   raw_y ∈ [0, 317]    (物理左右方向, 左=0 右=317)
// 映射到横屏 320×240:
//   屏幕X(0~319) = raw_y                         (直接对应, 范围吻合)
//   屏幕Y(0~239) = (212 - raw_x) * 239 / 187     (翻转 + 线性缩放)
bool readTouch(int *x, int *y) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);                          // 寄存器 0x02 起 = 触摸点1数据
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return false;
    uint8_t buf[6];
    for (int i = 0; i < 6; i++) buf[i] = Wire.read();
    if (buf[0] == 0) return false;             // buf[0]=触摸点数, 0=无触摸
    int raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    int raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    // 横屏坐标变换 (实测校准)
    *x = raw_y;
    *y = (212 - raw_x) * 239 / 187;
    if (*x < 0) *x = 0;  if (*x > 319) *x = 319;
    if (*y < 0) *y = 0;  if (*y > 239) *y = 239;
    return true;
}

// 屏幕开关 (屏幕故障时可注释掉, 跳过显示初始化)
#define ENABLE_DISPLAY

#include "radar_view.h"
#include "status_view.h"
#include "config_view.h"
#include "alert_sound.h"

RadarView radarView;
StatusView statusView;
ConfigView configView;
AlertSound alertSound;

// ============ 视图分发表 (数组替 switch, 加页面只需加一行) ============
// 所有视图继承 BaseView, 通过虚函数统一调用 draw/markDirty/onEnter/handleTouch.
BaseView* pages[] = { &radarView, &statusView, &configView };
const int totalPages = sizeof(pages) / sizeof(pages[0]);

// ============ 触摸/切页状态 ============
int currentPage = 0;        // 0=雷达图 1=状态 2=配置
int lastTouchX = -1, lastTouchY = -1;

// ============ SETUP ============
void setup() {
    Serial.begin(115200);
    delay(3000);  // ⚠ USB 供电需 3s 延迟让电源稳定, 否则 LovyanGFX + 背光初始化时掉电重启
    Serial.println("\n=== ebike-bsd C3 终端 " FW_VERSION " ===");
    Serial.println("Hardware: 立创实战派 ESP32-C3");

    // OTA 主动回滚保护: 必须在所有可能 panic 的初始化之前.
    // 若本槽是新 OTA 槽且本次是第 N 次尝试 setup, 超阈值则强制回滚到上一好槽.
    c3OtaBootGuardBegin();

    // UART 链路 (接主控) — P1 阶段核心, 必须先于屏幕验证
    netLink.init();

    // 屏幕 (ENABLE_DISPLAY: 第1步验证屏幕点亮 + 颜色/方向)
#ifdef ENABLE_DISPLAY
    // ⚠ 先初始化屏幕和 I2C, 再开背光 (减少 USB 供电下启动峰值电流)
    // 注: 不启用 DMA (initDMA), 实测 ESP32-C3 rev0.4 + USB 供电下 DMA 连续传输会导致掉电重启
    lcd.init();

    // I2C 总线初始化 (触摸 FT6336 / ES8311 / 传感器共用, SDA=0/SCL=1)
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(100000);

    // 背光: 实战派 C3 的背光 GPIO2 是低电平点亮 (官方 BK_LIGHT_ON_LEVEL=0)
    //   LovyanGFX 的 Light_PWM 默认高电平, 会反向. 直接用 digitalWrite 手动拉低.
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW);   // 低电平点亮背光

    Serial.printf("[LCD] %dx%d 初始化完成\n", lcd.width(), lcd.height());
#else
    Serial.println("[INIT] 屏幕未启用. 定义 ENABLE_DISPLAY 开启屏幕");
#endif

    // ES8311 报警音 (I2C 总线需在 lcd.init 之后, 共用 SDA=0/SCL=1)
    alertSound.init();

    Serial.printf("[INIT] 页面数: %d\n", totalPages);

    // OTA 回滚保护: 若本槽是 OTA 升级后首次启动且系统已就绪, 标记 valid 取消回滚挂起.
    // (若新固件在此前 panic, bootloader 下次重启自动回滚到上一好槽)
    c3OtaMarkValid();
}

// ============ LOOP ============
// 触摸调试模式开关: 注释掉恢复正常界面, 取消注释只画触摸点坐标 (排查 FT6336 映射)
// #define TOUCH_DEBUG

void loop() {
    // 1. 读 UART, 更新 netLink.state (含 OTA 帧接收)
    netLink.update();

    // OTA 升级进行中: 全屏显示进度, 跳过正常视图 + 报警音 (避免升级期间 SPI 总线竞争)
    if (c3OtaProgress.active) {
        // ⚠ 超时保护: 15 秒没收到分块 → 判定中断 → 自动退出 OTA 模式, 恢复正常接收
        // (主控崩溃/UART 断线等场景, 防止卡死在升级界面永远无法恢复)
        if (c3OtaProgress.lastChunkMs > 0 && millis() - c3OtaProgress.lastChunkMs > 15000) {
            Serial.println(F("[OTA] 超时 (15s 无数据), 自动退出升级模式"));
            c3OtaProgress.active = false;
            // 继续执行下面的正常视图处理
        } else {
#ifdef ENABLE_DISPLAY
            drawOtaOverlay();
#endif
            delay(100);
            return;
        }
    }

#ifdef ENABLE_DISPLAY
    // 2. 触摸处理 (切页 / 配置页调参)
    handleTouch();

    // 3. 按当前页绘制 (虚函数分发, 替 switch+ifdef)
    pages[currentPage]->draw(netLink.state);
#endif

    // 4. 报警音同步
    alertSound.update(netLink.state.bz_mode);

    delay(66);   // ~15fps 刷新 (主控 $S 推送 10Hz, 无需更快; 减少 SPI 总线占用防撕裂)
}

// ============ OTA 升级进度覆盖层 ============
#ifdef ENABLE_DISPLAY
// 适配: lgfx 无 mute 便捷色, 返回暗灰 (与各 view 的 MUTE_COLOR 风格一致)
inline uint16_t lgxcolor888_mute() { return lgfx::color888(139, 148, 158); }

void drawOtaOverlay() {
    static int lastPct = -1;
    int pct = c3OtaProgress.percent;
    // 仅在百分比变化时重绘 (减少 SPI 刷新, 节省带宽 + 防撕裂)
    if (pct == lastPct) return;
    lastPct = pct;

    lcd.startWrite();
    lcd.fillScreen(lgfx::color888(13, 17, 23));   // 与 Web UI 一致的深色底
    lcd.setTextColor(lgfx::color888(88, 166, 255));
    lcd.setTextSize(2);
    lcd.setTextDatum(textdatum_t::top_center);
    lcd.drawString("固件升级中", lcd.width() / 2, 50);

    lcd.setTextColor(lgfx::color888(201, 209, 217));
    lcd.setTextSize(1);
    lcd.drawString(String("目标: ") + c3OtaProgress.version, lcd.width() / 2, 90);
    lcd.drawString(String(c3OtaProgress.curSeq) + " / " + c3OtaProgress.totalSeq + " 块",
                   lcd.width() / 2, 110);

    // 进度条
    int barW = lcd.width() - 60, barH = 16;
    int barX = 30, barY = 150;
    lcd.drawRect(barX, barY, barW, barH, lgfx::color888(48, 54, 61));
    int fillW = (barW - 2) * pct / 100;
    lcd.fillRect(barX + 1, barY + 1, fillW, barH - 2, lgfx::color888(88, 166, 255));
    lcd.setTextColor(lgfx::color888(63, 185, 80));
    lcd.setTextDatum(textdatum_t::top_center);
    lcd.drawString(String(pct) + "%", lcd.width() / 2, barY + barH + 8);

    lcd.setTextColor(lgxcolor888_mute());
    lcd.drawString("升级期间不要断电", lcd.width() / 2, lcd.height() - 24);
    lcd.endWrite();
}
#endif

// 切页后调用, 强制新页面重绘 (虚函数分发; ConfigView 的 onEnter 会触发主控查询)
void markCurrentDirty() {
    pages[currentPage]->markDirty();
    pages[currentPage]->onEnter(netLink.state);
}

// 翻页按钮命中检测 (⚠ NAV_BTN_W/H 定义在 lgfx_config.hpp, 与各 view 一致)
bool hitNavPrev(int x, int y) { return x < NAV_BTN_W && y < NAV_BTN_H; }
bool hitNavNext(int x, int y) { return x >= lcd.width() - NAV_BTN_W && y < NAV_BTN_H; }

void handleTouch() {
    // 触摸扫描节流: 每 50ms 读一次 (足够灵敏, 又减少 I2C 占用避免与显示 SPI 竞争)
    static unsigned long lastScan = 0;
    unsigned long now = millis();
    if (now - lastScan < 50) return;
    lastScan = now;

    int tx, ty;
    bool touched = readTouch(&tx, &ty);

#ifdef TOUCH_DEBUG
    if (touched) {
        lcd.startWrite();
        lcd.fillCircle(tx, ty, 5, lgfx::color888(255, 255, 255));
        lcd.setTextColor(lgfx::color888(63, 185, 80), lgfx::color888(0, 0, 0));
        lcd.setTextSize(1);
        lcd.setCursor(4, 20);
        lcd.printf("scr:%d,%d", tx, ty);
        lcd.endWrite();
        Serial.printf("[TOUCH] scr=%d,%d\n", tx, ty);
    }
    return;
#endif

    // 边沿触发: 只在 "无触摸→有触摸" 的瞬间响应一次, 手指按住期间不重复触发
    // (FT6336 持续报点, 用旧的时间防抖会吞掉稳定后的有效点击)
    static bool wasPressed = false;
    if (!touched) { wasPressed = false; return; }
    if (wasPressed) return;        // 仍在按下状态, 忽略 (等松开后才能再触发)
    wasPressed = true;

    bool handled = false;

    // 翻页按钮优先 (左上 ‹ / 右上 ›), 在所有页面都生效
    if (hitNavPrev(tx, ty)) {
        currentPage = (currentPage - 1 + totalPages) % totalPages;
        handled = true;
        Serial.printf("[TOUCH] page -> %d\n", currentPage);
        markCurrentDirty();
    } else if (hitNavNext(tx, ty)) {
        currentPage = (currentPage + 1) % totalPages;
        handled = true;
        Serial.printf("[TOUCH] page -> %d\n", currentPage);
        markCurrentDirty();
    }
    else {
        // 当前页的触摸事件 (虚函数分发; RadarView/StatusView 默认返回 false)
        handled = pages[currentPage]->handleTouch(tx, ty);
    }

    (void)handled;
}
