// ============================================================
//  lgfx_config.hpp - LovyanGFX 配置 (立创·实战派 ESP32-C3)
//  屏幕: ST7789, 2.0寸, 320x240, SPI
//  触摸: FT6336, I2C 0x38
//  引脚来源: zhuhai-esp/XD-ESP32C3-AIoT 社区仓库 + LCSC BOM
// ============================================================
#pragma once
#include <LovyanGFX.hpp>

// 实战派 C3 屏幕引脚 (SPI)
#define LCD_MOSI  5
#define LCD_MISO  -1    // 屏幕只写不读
#define LCD_SCLK  3
#define LCD_CS    4
#define LCD_DC    6
#define LCD_RST   -1    // 接 EN 复位, 无独立 RST
#define LCD_BL    2     // 背光

// 触摸引脚 (I2C, 与 ES8311/QMI8658 共用总线)
#define TOUCH_SDA 8
#define TOUCH_SCL 9
#define TOUCH_INT -1

// 自定义屏幕配置类
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
    lgfx::Touch_FT5x06  _touch;   // FT6336 兼容 FT5x06 驱动

public:
    LGFX() {
        {   // 总线 SPI 配置
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;       // ESP32-C3: SPI2
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;      // 40MHz
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
            cfg.invert   = true;            // ST7789 需要反色
            cfg.rgb_order = false;          // BGR
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {   // 背光 PWM
            auto cfg = _light.config();
            cfg.pin_bl = LCD_BL;
            cfg.freq = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        {   // 触摸 FT6336 (通过 _cfg 配置 I2C 引脚, 再 setTouch)
            auto cfg = _touch.config();
            cfg.i2c_addr = 0x38;
            cfg.x_min = 0;
            cfg.x_max = 319;
            cfg.y_min = 0;
            cfg.y_max = 239;
            cfg.pin_sda = TOUCH_SDA;
            cfg.pin_scl = TOUCH_SCL;
            cfg.freq = 400000;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};
