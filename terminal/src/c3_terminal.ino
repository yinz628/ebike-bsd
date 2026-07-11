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

// 全局对象
LGFX lcd;
UartLink netLink;

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

// 分阶段开关 (开发时按需注释, 验证完逐个打开)
#define ENABLE_DISPLAY        // P1: 屏幕初始化
#define ENABLE_RADAR_VIEW     // P2: 雷达扇形图
#define ENABLE_STATUS_VIEW    // P3: 状态页
#define ENABLE_CONFIG_VIEW    // P3: 配置页 + 触摸
#define ENABLE_ALERT_SOUND    // P4: ES8311 报警音 (功放默认关闭, 播放时才使能)

// 主控离线时显示模拟数据 (验证视图用; 接上主控后自动切换真实数据)
// 注: 生产环境注释掉, 避免主控离线时显示假目标误导用户
// #define DEMO_WHEN_OFFLINE

// 心跳日志开关 (每 2 秒打印 [HB] 在线状态; 调试时取消注释, 日常运行关闭省串口开销)
// #define ENABLE_HB_LOG

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
    delay(3000);  // ⚠ USB 供电需 3s 延迟让电源稳定, 否则 LovyanGFX + 背光初始化时掉电重启
    Serial.println("\n=== ebike-bsd C3 终端 V0.1 ===");
    Serial.println("Hardware: 立创实战派 ESP32-C3");

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

    // 主控离线时加载模拟数据 (验证雷达图视图)
#ifdef DEMO_WHEN_OFFLINE
    netLink.loadDemoData();
    Serial.println("[DEMO] 已加载模拟目标数据 (主控离线时显示)");
#endif

    // ES8311 报警音 (I2C 总线需在 lcd.init 之后, 共用 SDA=0/SCL=1)
#ifdef ENABLE_ALERT_SOUND
    alertSound.init();
#endif

    updatePages();
    Serial.printf("[INIT] 页面数: %d\n", totalPages);
}

// ============ LOOP ============
// 触摸调试模式开关: 注释掉恢复正常界面, 取消注释只画触摸点坐标 (排查 FT6336 映射)
// #define TOUCH_DEBUG

void loop() {
    // 1. 读 UART, 更新 netLink.state
    netLink.update();

#ifdef ENABLE_DISPLAY
    // 2. 触摸处理 (切页 / 配置页调参)
    handleTouch();

#ifndef TOUCH_DEBUG
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
#endif

    // 4. 报警音同步
#ifdef ENABLE_ALERT_SOUND
    alertSound.update(netLink.state.bz_mode);
#endif

    // 心跳 (每 2 秒, 确认固件在跑 + 显示在线状态; 仅调试时开启)
#ifdef ENABLE_HB_LOG
    static unsigned long lastHb = 0;
    if (millis() - lastHb > 2000) {
        lastHb = millis();
        Serial.printf("[HB] online=%d last_frame=%lums ago\n",
                      netLink.state.online,
                      netLink.state.online ? (millis() - netLink.state.last_frame_ms) : 0);
    }
#endif

    delay(66);   // ~15fps 刷新 (主控 $S 推送 10Hz, 无需更快; 减少 SPI 总线占用防撕裂)
}

// ============ 触摸处理 ============
// 切页后调用, 强制新页面重绘 + 配置页触发主控查询
void markCurrentDirty() {
#ifdef ENABLE_RADAR_VIEW
    if (currentPage == 0) radarView.markDirty();
#endif
#ifdef ENABLE_STATUS_VIEW
    if (currentPage == 1) statusView.markDirty();
#endif
#ifdef ENABLE_CONFIG_VIEW
    if (currentPage == 2) configView.onEnter(netLink.state);
#endif
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
#ifdef ENABLE_CONFIG_VIEW
    else if (currentPage == 2) {
        handled = configView.handleTouch(tx, ty);
    }
#endif

    (void)handled;
}
