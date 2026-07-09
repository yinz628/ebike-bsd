// ============================================================
//  lgfx_config.hpp - LovyanGFX 配置 (立创·实战派 ESP32-C3)
//  屏幕: ST7789, 2.0寸, 320x240, SPI
//  触摸: FT6336, I2C 0x38
//  引脚来源: 立创官方 IDF 例程 (esp32-c3/例程代码（IDF）/)
//    - 屏幕引脚对照 07-spi_lcd / 08-spi_lcd_touch
//    - 触摸 I2C 对照 08-spi_lcd_touch/myi2c.h (SDA=0, SCL=1)
// ============================================================
#pragma once
#include <LovyanGFX.hpp>

// 实战派 C3 屏幕引脚 (SPI) — 与官方例程完全一致
#define LCD_MOSI  5
#define LCD_MISO  -1    // 屏幕只写不读
#define LCD_SCLK  3
#define LCD_CS    4
#define LCD_DC    6
#define LCD_RST   -1    // 接 EN 复位, 无独立 RST
#define LCD_BL    2     // 背光

// 触摸引脚 (I2C, 与 ES8311/QMI8658/GXHTC3 共用总线)
// ⚠ 官方 IDF 例程确认: 实战派 C3 的板载 I2C 总线是 GPIO0(SDA)/GPIO1(SCL)
#define TOUCH_SDA 0
#define TOUCH_SCL 1
#define TOUCH_ADDR 0x38   // FT6336 I2C 地址

// 自定义屏幕配置类
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    // 注: 背光 GPIO2 不用 Light_PWM (实战派 C3 背光低电平点亮, PWM 极性反向)
    //     改为在 c3_terminal.ino setup 里 digitalWrite(2, LOW) 手动点亮

public:
    LGFX() {
        {   // 总线 SPI 配置
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;       // ESP32-C3: SPI2
            cfg.spi_mode = 0;
            cfg.freq_write = 20000000;      // 20MHz (官方例程值, 先稳定排查; 验证后可升 40MHz)
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = true;          // MISO 不用
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = LCD_SCLK;
            cfg.pin_mosi = LCD_MOSI;
            cfg.pin_miso = LCD_MISO;
            cfg.pin_dc   = LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {   // 面板配置
            auto cfg = _panel.config();
            cfg.pin_cs   = LCD_CS;
            cfg.pin_rst  = LCD_RST;
            cfg.memory_width  = 240;
            cfg.memory_height = 320;
            cfg.panel_width   = 240;
            cfg.panel_height  = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 1;        // 横屏 320x240
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable = false;
            cfg.invert   = true;            // ST7789 需要反色 (官方 esp_lcd_panel_invert_color(true))
            cfg.rgb_order = true;           // RGB (官方 LCD_RGB_ELEMENT_ORDER_RGB; 若红蓝互换改 false)
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        // 注: 背光控制已移除 Light_PWM, 改用 digitalWrite(2, LOW) 手动点亮
        // 注: 触摸 FT6336 不用 LovyanGFX 内置 FT5x06 驱动 (其横屏旋转坐标有偏移),
        //     改用 c3_terminal.ino 里裸 I2C 读取 + 手动旋转变换 (与 v2.7-c3-display 一致)
        setPanel(&_panel);
    }
};

// ============================================================
//  共用 UI 组件
// ============================================================

// 翻页按钮尺寸 (⚠ 须与 c3_terminal.ino 的 hitNavPrev/hitNavNext 一致)
#define NAV_BTN_W 30
#define NAV_BTN_H 24

// 绘制翻页按钮: isNext=false 画左上 <, true 画右上 >
// 模板兼容 LGFX 和 LGFX_Sprite (两者都有 width/fillRoundRect/setTextSize)
template <typename T>
inline void drawNavBtn(T &g, bool isNext) {
    int w = g.width();
    int x = isNext ? (w - NAV_BTN_W) : 0;
    int y = 0;
    uint32_t bg  = lgfx::color888(33, 38, 53);
    uint32_t fg  = lgfx::color888(88, 166, 255);
    g.fillRoundRect(x, y, NAV_BTN_W, NAV_BTN_H, 4, bg);
    g.drawRoundRect(x, y, NAV_BTN_W, NAV_BTN_H, 4, fg);
    g.setTextColor(fg, bg);
    g.setTextSize(2);
    const char *arrow = isNext ? ">" : "<";
    g.setCursor(x + (NAV_BTN_W - 12) / 2, y + 4);
    g.print(arrow);
}
