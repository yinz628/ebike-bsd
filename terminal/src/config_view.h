// ============================================================
//  config_view.h - 参数配置页 (Page 2) + FT6336 触摸调参
//  布局移植自 v2.7-c3-display: -/+ 按钮在中间(x=115/161), 不靠右边缘
//  分 3 个 tab: RCW(6) / TURN(2) / SYS(3 + WiFi开关)
//  底部: [SAVE] + [REFRESH] (REFRESH 查询主控当前配置)
//  切到 SYS tab 时自动 requestConfig 获取 WiFi 状态
//  ⚠ 屏幕用英文 (LovyanGFX 无中文字库); key 与主控 terminal_link.h 对齐
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"

extern LGFX lcd;
extern UartLink netLink;

class ConfigView {
private:
    struct Param {
        const char *key;
        const char *label;
        int value;
        int step;
        int vmin, vmax;
        const char *unit;
        int  cfgIdx;   // 对应 state.cfg[] 的索引 (GETCFG 响应顺序), -1=无
    };

    // params 顺序与主控 GETCFG 响应 ($CFG,v0..v13) 对齐 (14 个值):
    //   0:rcw_low 1:rcw_speed 2:rcw_range 3:rcw_lateral 4:rcw_hold 5:rcw_lflash 6:rcw_flash
    //   7:turn_speed 8:turn_range 9:turn_lateral 10:beep_cool 11:det_range 12:sensitivity 13:wifi_on
    static const int NPARAM = 13;
    Param params[NPARAM] = {
        // RCW
        {"rcw_low",     "LOW_V",  2,    1,   0, 5,      "m/s", 0},
        {"rcw_speed",   "HI_V",   3,    1,   1, 10,     "m/s", 1},
        {"rcw_range",   "RANGE",  25,   5,   5, 50,     "m",   2},
        {"rcw_lateral", "LAT",    3,    1,   1, 10,     "m",   3},
        {"rcw_hold",    "HOLD",   3000, 500, 500,10000, "ms",  4},
        {"rcw_lflash",  "L_FLSH", 500,  100, 100,2000,  "ms",  5},
        {"rcw_flash",   "H_FLSH", 125,  25,  50, 500,   "ms",  6},
        // TURN
        {"turn_speed",  "T_SPD",  2,    1,   0, 10,     "m/s", 7},
        {"turn_range",  "T_RNG",  30,   5,   5, 50,     "m",   8},
        {"turn_lateral","T_LAT",  3,    1,   1, 10,     "m",   9},
        // SYS
        {"beep_cool",   "BEEP_CD",5000, 500, 1000,10000,"ms",  10},
        {"det_range",   "D_RNG",  30,   5,   5, 50,     "m",   11},
        {"sensitivity", "SENS",   2,    1,   0, 10,     "",    12},
    };

    // tab 分组: 0=SYS, 1=RCW1, 2=RCW2, 3=TURN (SYS 放首页, 进配置页先看系统状态)
    // params 数组顺序不变, tabStart/tabCount 按显示顺序重映射
    static const int NTAB = 4;
    // SYS(10,11,12) RCW1(0,1,2,3) RCW2(4,5,6) TURN(7,8,9)
    const int tabStart[NTAB] = {10, 0, 4, 7};
    const int tabCount[NTAB] = {3, 4, 3, 3};
    const char *tabName[NTAB] = {"SYS", "RCW1", "RCW2", "TURN"};

    int tab = 0;
    int selected = 0;
    bool dirty = false;
    bool _needsDraw = true;
    unsigned long refreshMsg = 0;   // REFRESH 反馈显示时间 (0=不显示)
    uint8_t _lastCfgSeq = 0;        // 上次同步的 cfg_seq (变化时才同步, 避免覆盖本地修改)

    // 布局: 行高 26px, 起始 y=36 (每页最多 4 行, 到 y=134, 底部按钮 y=214 充裕)
    int rowY(int i) { return 36 + i * 26; }
    int paramIdx(int rowInTab) { return tabStart[tab] + rowInTab; }

    static const int BTN_MINUS_X = 115, BTN_PLUS_X = 161;
    static const int BTN_W = 44, BTN_H = 22;

    bool hitMinus(int tx, int ty, int y) {
        return tx >= BTN_MINUS_X && tx <= BTN_MINUS_X + BTN_W && ty >= y - 2 && ty <= y - 2 + BTN_H;
    }
    bool hitPlus(int tx, int ty, int y) {
        return tx >= BTN_PLUS_X && tx <= BTN_PLUS_X + BTN_W && ty >= y - 2 && ty <= y - 2 + BTN_H;
    }

public:
    void markDirty() { _needsDraw = true; }

    // 切到本页 / 切 tab 时调用: 同步主控配置 + 进入 SYS 时查询 WiFi
    void onEnter(const TerminalState &st) {
        syncFromMaster(st);
        if (tab == 0) netLink.requestConfig();   // SYS tab (首页) 需要最新 WiFi 状态
        _needsDraw = true;
    }

    void draw(const TerminalState &st) {
        // REFRESH 反馈: 2 秒后需要清除文字, 触发一次重绘 (不持续重绘, 避免闪屏)
        if (refreshMsg && millis() - refreshMsg >= 2000) {
            refreshMsg = 0;
            _needsDraw = true;   // 清除 REFRESHED! 文字
        }
        if (!_needsDraw) return;
        _needsDraw = false;

        // 收到新的主控配置时同步一次 (仅 REFRESH/onEnter 触发, 不覆盖用户编辑)
        // cfg_seq 变化 = 主控回了新的 $CFG, 此时才同步
        if (st.cfg_valid && st.cfg_seq != _lastCfgSeq) {
            syncFromMaster(st);
            _lastCfgSeq = st.cfg_seq;
            dirty = false;   // 同步后清除未保存标记 (值与主控一致)
        }

        lcd.startWrite();
        lcd.fillScreen(lgfx::color888(13, 17, 23));

        // 顶部标题 + tab 名
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setTextSize(1);
        lcd.setCursor(36, 6);
        lcd.printf("[CFG] %s (%d/%d)", tabName[tab], tab + 1, NTAB);
        if (dirty) {
            lcd.setTextColor(lgfx::color888(210, 153, 34));
            lcd.print(" *");
        }

        // 分隔线
        lcd.drawLine(0, 34, lcd.width(), 34, lgfx::color888(48, 54, 61));

        // 参数行
        int n = tabCount[tab];
        for (int i = 0; i < n; i++) {
            int y = rowY(i);
            Param &p = params[paramIdx(i)];
            if (i == selected) {
                lcd.fillRect(0, y - 2, lcd.width(), 28, lgfx::color888(22, 33, 50));
            }
            lcd.setTextColor(lgfx::color888(201, 209, 217));
            lcd.setTextSize(1);
            lcd.setCursor(5, y + 4);
            lcd.print(p.label);
            drawAdjustBtn(BTN_MINUS_X, y - 2, "-", lgfx::color888(248, 81, 73));
            drawAdjustBtn(BTN_PLUS_X, y - 2, "+", lgfx::color888(63, 185, 80));
            lcd.setTextColor(lgfx::color888(255, 255, 255));
            lcd.setTextSize(2);
            lcd.setCursor(215, y);
            lcd.printf("%d", p.value);
            lcd.setTextSize(1);
            lcd.setTextColor(lgfx::color888(139, 148, 158));
            lcd.setCursor(270, y + 4);
            lcd.print(p.unit);
        }

        // SYS tab (首页): WiFi 开关行 (在参数行之后)
        if (tab == 0) {
            int wy = rowY(tabCount[0]);   // 参数行之后的位置
            // 标签
            lcd.setTextColor(lgfx::color888(201, 209, 217));
            lcd.setTextSize(1);
            lcd.setCursor(5, wy + 4);
            lcd.print("WiFi");
            // 开关按钮 (x=115, 宽 90, 显示 ON/OFF)
            bool won = netLink.state.wifi_on;
            uint32_t wc = won ? lgfx::color888(63, 185, 80) : lgfx::color888(248, 81, 73);
            lcd.fillRoundRect(BTN_MINUS_X, wy - 2, 90, BTN_H, 4, wc);
            lcd.setTextColor(lgfx::color888(255, 255, 255));
            lcd.setTextSize(2);
            lcd.setCursor(BTN_MINUS_X + 20, wy + 4);
            lcd.print(won ? "ON" : "OFF");
            // 状态文字 (主控离线时提示)
            if (!netLink.isOnline()) {
                lcd.setTextSize(1);
                lcd.setTextColor(lgfx::color888(139, 148, 158));
                lcd.setCursor(215, wy + 4);
                lcd.print("(offline)");
            }
        }

        // 底部按钮 (y=214): [SAVE] + [REFRESH]
        // ⚠ 都放左半屏到中间, 避开右边缘触摸死区 (raw_y>290 时坐标不准)
        int by = 214;
        // SAVE (x=5~95, 绿色/灰)
        lcd.fillRoundRect(5, by, 90, 22, 4,
                          dirty ? lgfx::color888(63, 185, 80) : lgfx::color888(48, 54, 61));
        lcd.setTextColor(dirty ? lgfx::color888(0, 0, 0) : lgfx::color888(139, 148, 158));
        lcd.setTextSize(1);
        lcd.setCursor(25, by + 7);
        lcd.print("[SAVE]");

        // REFRESH (x=100~190, 蓝色) — 查询主控当前配置
        lcd.fillRoundRect(100, by, 90, 22, 4, lgfx::color888(88, 166, 255));
        lcd.setTextColor(lgfx::color888(0, 0, 0));
        lcd.setCursor(112, by + 7);
        lcd.print("[REFRESH]");

        // REFRESH 反馈 (2 秒内显示绿色 REFRESHED!)
        if (refreshMsg && millis() - refreshMsg < 2000) {
            lcd.setTextColor(lgfx::color888(63, 185, 80), lgfx::color888(13, 17, 23));
            lcd.setTextSize(2);
            lcd.setCursor(205, 214);
            lcd.print("REFRESHED!");
        } else if (refreshMsg) {
            refreshMsg = 0;   // 超时清除
        }

        // 顶部翻页按钮
        drawNavBtn(lcd, false);
        drawNavBtn(lcd, true);

        lcd.endWrite();
    }

    bool handleTouch(int tx, int ty) {
        int n = tabCount[tab];

        // 底部按钮优先判断 (y >= 205, 避免被参数行选中拦截)
        // SAVE (x=0~95)
        if (ty >= 205 && tx <= 95) {
            if (dirty) {
                // 逐个发送参数, 最后发 SAVE 触发主控持久化到 NVS
                for (int i = 0; i < NPARAM; i++) {
                    netLink.sendConfig("", params[i].key, params[i].value);
                    delay(20);
                }
                netLink.sendSave();   // 触发 config.saveToNVS() + radar.setBSDMode()
                dirty = false; _needsDraw = true;
                Serial.println("[CFG] sent all params + SAVE to master");
            }
            return true;
        }
        // REFRESH (x=100~190)
        if (ty >= 205 && tx >= 100 && tx <= 190) {
            netLink.requestConfig();
            refreshMsg = millis();   // 显示反馈 2 秒
            Serial.println("[CFG] refresh requested");
            _needsDraw = true;
            return true;
        }

        // -/+ 按钮
        for (int i = 0; i < n; i++) {
            int y = rowY(i);
            if (hitMinus(tx, ty, y)) { adjustParam(paramIdx(i), -1); selected = i; _needsDraw = true; return true; }
            if (hitPlus(tx, ty, y))  { adjustParam(paramIdx(i), +1); selected = i; _needsDraw = true; return true; }
        }

        // SYS tab (首页): WiFi 开关
        if (tab == 0) {
            int wy = rowY(tabCount[0]);
            if (tx >= BTN_MINUS_X && tx <= BTN_MINUS_X + 90 && ty >= wy - 2 && ty <= wy - 2 + BTN_H) {
                bool nw = !netLink.state.wifi_on;
                netLink.sendWifi(nw);
                netLink.state.wifi_on = nw;   // 立即更新 UI (主控会确认)
                _needsDraw = true;
                Serial.printf("[CFG] WiFi -> %s\n", nw ? "ON" : "OFF");
                return true;
            }
        }

        // 点击参数行标签区 = 选中
        for (int i = 0; i < n; i++) {
            int y = rowY(i);
            if (ty >= y - 2 && ty < y + 26 && tx < BTN_MINUS_X) {
                if (selected != i) { selected = i; _needsDraw = true; }
                return true;
            }
        }

        // tab 切换: 点击标题区左右半
        if (ty >= 0 && ty < 34 && tx >= NAV_BTN_W && tx < lcd.width() - NAV_BTN_W) {
            if (tx < lcd.width() / 2) tab = (tab - 1 + NTAB) % NTAB;
            else                       tab = (tab + 1) % NTAB;
            selected = 0;
            if (tab == 0) netLink.requestConfig();   // 进入 SYS (首页) 查询 WiFi
            _needsDraw = true;
            Serial.printf("[CFG] tab -> %s\n", tabName[tab]);
            return true;
        }

        return false;
    }

private:
    // 从主控回传的 cfg[] 同步到 params[].value
    void syncFromMaster(const TerminalState &st) {
        if (!st.cfg_valid) return;
        for (int i = 0; i < NPARAM; i++) {
            int ci = params[i].cfgIdx;
            if (ci >= 0 && ci < 14) params[i].value = st.cfg[ci];
        }
    }

    void adjustParam(int idx, int dir) {
        Param &p = params[idx];
        int nv = p.value + dir * p.step;
        if (nv < p.vmin) nv = p.vmin;
        if (nv > p.vmax) nv = p.vmax;
        if (nv != p.value) {
            p.value = nv;
            dirty = true;
            Serial.printf("[CFG] %s = %d\n", p.key, nv);
        }
    }

    void drawAdjustBtn(int x, int y, const char *txt, uint32_t color) {
        lcd.fillRoundRect(x, y, BTN_W, BTN_H, 4, color);
        lcd.setTextColor(lgfx::color888(255, 255, 255));
        lcd.setTextSize(2);
        lcd.setCursor(x + (BTN_W - 12) / 2, y + (BTN_H - 16) / 2);
        lcd.print(txt);
    }
};
