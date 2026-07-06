// ============================================================
//  oled_display.h - OLED 显示驱动 (可选, 需要 Adafruit SSD1306 库)
//  用途: 显示单雷达 BSD 检测结果、角度分布、转向灯状态
//  使用: platformio.ini 中取消注释 ENABLE_OLED build_flag
// ============================================================
#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>

#ifdef ENABLE_OLED

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_WIDTH     128
#define OLED_HEIGHT     64
#define OLED_ADDRESS    0x3C
#define OLED_RST_PIN    -1

// 角度方向标识
#define ANGLE_ZONE_NONE  0
#define ANGLE_ZONE_LEFT  1
#define ANGLE_ZONE_RIGHT 2
#define ANGLE_ZONE_CENTER 3

class OLEDDisplay {
private:
    Adafruit_SSD1306 _display;
    bool _initialized;

public:
    OLEDDisplay() : _display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RST_PIN),
                    _initialized(false) {}

    bool begin() {
        Wire.begin(21, 22);
        _initialized = _display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
        
        if (_initialized) {
            _display.clearDisplay();
            _display.setTextSize(1);
            _display.setTextColor(SSD1306_WHITE);
            _display.setCursor(0, 0);
            _display.println("BSD V2.1");
            _display.println("Single Radar");
            _display.println("Booting...");
            _display.display();
            delay(1000);
        }
        return _initialized;
    }

    // 主更新函数
    // targets: 目标总数
    // nLeft, nRight: 左右盲区目标数
    // angles[]: 各目标角度 (用于显示方向小箭头)
    // turnLeft/Right/Hazard: 转向灯状态
    void update(bool turnLeft, bool turnRight, bool hazard,
                bool bsdL, bool bsdR,
                int totalTargets, int nLeft, int nRight,
                const int8_t *angles, int numAngles) {
        if (!_initialized) return;
        
        _display.clearDisplay();
        
        // 标题行
        _display.setTextSize(1);
        _display.setCursor(0, 0);
        _display.print("BSD ");
        _display.print(totalTargets);
        _display.print("tgt");
        if (bsdL) _display.print(" L*");
        if (bsdR) _display.print(" R*");
        
        // 分割线
        _display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
        
        // 盲区状态条: 左 | 中 | 右
        // L 盲区
        _display.setCursor(4, 14);
        _display.print("L:");
        _display.print(nLeft);
        if (bsdL) {
            _display.fillRect(0, 24, 20, 6, SSD1306_WHITE);  // 填充条
        } else {
            _display.drawRect(0, 24, 20, 6, SSD1306_WHITE);  // 空心框
        }
        
        // C 正后方
        int nCenter = totalTargets - nLeft - nRight;
        _display.setCursor(52, 14);
        _display.print("C:");
        _display.print(nCenter);
        
        // R 盲区
        _display.setCursor(102, 14);
        _display.print("R:");
        _display.print(nRight);
        if (bsdR) {
            _display.fillRect(108, 24, 20, 6, SSD1306_WHITE);
        } else {
            _display.drawRect(108, 24, 20, 6, SSD1306_WHITE);
        }
        
        // 角度分布小图 (水平条, 标出各目标角度位置)
        // 范围 -40° ~ +40°, 映射到 0~128 像素
        for (int i = 0; i < numAngles && i < 8; i++) {
            int px = map(constrain(angles[i], -40, 40), -40, 40, 4, 124);
            _display.drawPixel(px, 38, SSD1306_WHITE);
            _display.drawPixel(px+1, 38, SSD1306_WHITE);
        }
        // 中心标记 (0°)
        _display.drawFastVLine(64, 35, 6, SSD1306_WHITE);
        
        // 转向灯状态 (大字)
        _display.setTextSize(2);
        
        if (turnLeft) {
            _display.setCursor(6, 44);
            _display.print("<<<");
        }
        if (turnRight) {
            _display.setCursor(78, 44);
            _display.print(">>>");
        }
        if (hazard) {
            _display.setCursor(26, 44);
            _display.print("HAZ");
        }
        
        _display.display();
    }
};

#else
// OLED 未启用时的桩实现
class OLEDDisplay {
public:
    bool begin() { return false; }
    void update(bool, bool, bool, bool, bool, int, int, int, const int8_t*, int) {}
};

#endif // ENABLE_OLED

#endif // OLED_DISPLAY_H
