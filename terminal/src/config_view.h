// ============================================================
//  config_view.h - 参数配置页 (Page 2) + FT6336 触摸调参
//  显示可调参数 + [</>] 按钮, 触摸修改后通过 UART 发 $C,key=value 到主控
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"

extern LGFX lcd;
extern UartLink netLink;

class ConfigView {
private:
    // 参数定义: group (UI 分组), key (主控 terminal_link.h 识别的键名),
    //           显示名, 当前值, 步长, 最小, 最大, 单位
    // ⚠ key 必须与主控 terminal_link.h applyCommand() 里的键名完全一致
    struct Param {
        const char *group;
        const char *key;
        const char *label;
        int value;
        int step;
        int vmin, vmax;
        const char *unit;
    };

    static const int NPARAM = 5;
    Param params[NPARAM] = {
        {"rcw",   "rcw_speed",  "RCW高警告速度", 3,    1, 1,  10, "m/s"},
        {"rcw",   "rcw_range",  "RCW距离上限",   25,   5, 5,  50, "m"},
        {"radar", "sensitivity","雷达灵敏度",     2,    1, 0,  10, "档"},
        {"rcw",   "rcw_low",    "RCW低警告速度", 2,    1, 0,   5, "m/s"},
        {"sys",   "beep_cool",  "蜂鸣冷却",      5000, 500, 1000, 10000, "ms"},
    };

    int selected = 0;   // 当前选中项索引
    bool dirty = false; // 有未保存修改

    // 布局: 每行高 32px, <> 按钮在每行右侧
    // 保存/出厂按钮在最底部
    int rowY(int i) { return 30 + i * 32; }

public:
    void draw(const TerminalState &st) {
        lcd.fillScreen(lgfx::color888(13, 17, 23));

        // 标题
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setTextSize(1);
        lcd.setCursor(8, 8);
        lcd.print("[ 参数配置 ]");
        if (dirty) {
            lcd.setTextColor(lgfx::color888(210, 153, 34));
            lcd.print(" *未保存");
        }

        // 参数行
        for (int i = 0; i < NPARAM; i++) {
            int y = rowY(i);
            // 选中项背景高亮
            if (i == selected) {
                lcd.fillRect(0, y, lcd.width(), 28, lgfx::color888(22, 33, 50));
            }
            // 标签
            lcd.setTextColor(lgfx::color888(201, 209, 217));
            lcd.setCursor(8, y + 4);
            lcd.print(params[i].label);

            // 值 + 单位
            lcd.setTextColor(lgfx::color888(88, 166, 255));
            lcd.setCursor(8, y + 16);
            lcd.printf("%d %s", params[i].value, params[i].unit);

            // </> 按钮 (右侧)
            drawBtn(lcd.width() - 60, y + 4, 24, 20, "<");
            drawBtn(lcd.width() - 30, y + 4, 24, 20, ">");
        }

        // 底部按钮: 保存 / 出厂重置
        int by = lcd.height() - 24;
        lcd.fillRoundRect(10, by, 120, 20, 4,
                          dirty ? lgfx::color888(88, 166, 255) : lgfx::color888(48, 54, 61));
        lcd.setTextColor(dirty ? lgfx::color888(13,17,23) : lgfx::color888(139, 148, 158));
        lcd.setCursor(40, by + 6);
        lcd.print("[保存]");

        lcd.fillRoundRect(lcd.width() - 130, by, 120, 20, 4, lgfx::color888(248, 81, 73));
        lcd.setTextColor(lgfx::color888(255, 255, 255));
        lcd.setCursor(lcd.width() - 118, by + 6);
        lcd.print("[出厂重置]");

        // 提示
        lcd.setTextColor(lgfx::color888(139, 148, 158));
        lcd.setCursor(8, by - 12);
        lcd.print("点击<>调值 顶部选行 保存生效");
    }

    // 触摸事件处理 (由 c3_terminal.ino handleTouch 调用)
    // 返回 true 表示消耗了触摸
    bool handleTouch(int tx, int ty) {
        // 检测点击哪个参数的 <> 按钮
        for (int i = 0; i < NPARAM; i++) {
            int y = rowY(i);
            int w = lcd.width();
            // < 按钮
            if (tx >= w - 60 && tx <= w - 36 && ty >= y + 4 && ty <= y + 24) {
                adjustParam(i, -1);
                selected = i;
                return true;
            }
            // > 按钮
            if (tx >= w - 30 && tx <= w - 6 && ty >= y + 4 && ty <= y + 24) {
                adjustParam(i, +1);
                selected = i;
                return true;
            }
        }

        // 点击参数行本身 = 选中 (不调值)
        for (int i = 0; i < NPARAM; i++) {
            int y = rowY(i);
            if (ty >= y && ty < y + 28 && tx < lcd.width() - 60) {
                selected = i;
                return true;
            }
        }

        // 保存按钮 (逐个 POST, 主控 handler 自动 saveToNVS + setBSDMode)
        int by = lcd.height() - 24;
        if (ty >= by && ty <= by + 20 && tx >= 10 && tx <= 130) {
            if (dirty) {
                for (int i = 0; i < NPARAM; i++) {
                    netLink.sendConfig(params[i].group, params[i].key, params[i].value);
                    delay(20);
                }
                dirty = false;
                Serial.println("[CFG] 已 POST 全部参数 (主控自动保存)");
            }
            return true;
        }

        // 出厂重置按钮
        if (ty >= by && ty <= by + 20 && tx >= lcd.width() - 130 && tx <= lcd.width() - 10) {
            netLink.sendFactoryReset();
            Serial.println("[CFG] 已发送出厂重置");
            return true;
        }

        return false;
    }

private:
    void adjustParam(int i, int dir) {
        int nv = params[i].value + dir * params[i].step;
        if (nv < params[i].vmin) nv = params[i].vmin;
        if (nv > params[i].vmax) nv = params[i].vmax;
        if (nv != params[i].value) {
            params[i].value = nv;
            dirty = true;
            Serial.printf("[CFG] %s.%s = %d\n", params[i].group, params[i].key, nv);
        }
    }

    void drawBtn(int x, int y, int w, int h, const char *txt) {
        lcd.drawRoundRect(x, y, w, h, 3, lgfx::color888(88, 166, 255));
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setCursor(x + w/2 - 3, y + 3);
        lcd.print(txt);
    }
};
