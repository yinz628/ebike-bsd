// ============================================================
//  status_view.h - 系统状态页 (Page 1)
//  显示: 连接状态/运行时间/雷达字节/转向/蜂鸣/各目标详情
//
//  防闪屏: 用脏标志 + 周期刷新. 仅当关键字段变化或每秒一次时重绘.
//          重绘时用 startWrite/endWrite 整帧提交, 避免 SPI 分段撕裂.
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"
#include "base_view.h"

extern LGFX lcd;

class StatusView : public BaseView {
private:
    unsigned long bootMs = 0;
    bool     _dirty = true;          // 首次进入强制画一次
    uint32_t _lastDrawSec = 0xFFFFFFFF;  // 上次绘制时的运行秒数 (运行时间每秒刷)

    // 缓存上次绘制用的关键字段摘要 (变化即重绘)
    uint8_t  _sum_obj = 0xFF, _sum_bz = 0xFF, _sum_turn = 0xFF;
    bool     _sum_online = false;
    uint32_t _sum_rx = 0xFFFFFFFF;

    // 计算状态摘要, 用于判断是否需要重绘
    // rx_bytes 量化到 KB (每 1024 字节才算变化), 避免每帧递增导致无谓重绘
    bool summaryChanged(const TerminalState &st) {
        return st.obj_num != _sum_obj
            || st.bz_mode != _sum_bz
            || st.turn    != _sum_turn
            || st.online  != _sum_online
            || (st.rx_bytes / 1024) != _sum_rx;
    }
    void saveSummary(const TerminalState &st) {
        _sum_obj = st.obj_num; _sum_bz = st.bz_mode; _sum_turn = st.turn;
        _sum_online = st.online; _sum_rx = st.rx_bytes / 1024;   // 存 KB 量化值
    }

public:
    void markDirty() override { _dirty = true; }   // 外部切页时调用, 强制重绘

    void draw(const TerminalState &st) override {
        if (bootMs == 0) bootMs = millis();

        // 运行时间每秒变化; 其他字段变化也触发; 否则不重绘
        uint32_t nowSec = (millis() - bootMs) / 1000;
        if (!_dirty && nowSec == _lastDrawSec && !summaryChanged(st)) {
            return;
        }
        _dirty = false;
        _lastDrawSec = nowSec;
        saveSummary(st);

        lcd.startWrite();
        lcd.fillScreen(lgfx::color888(13, 17, 23));

        // 标题
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setTextSize(1);
        lcd.setCursor(36, 6);
        lcd.print("[ STATUS ]");

        // 连接状态
        lcd.setTextColor(st.online ? lgfx::color888(63, 185, 80) : lgfx::color888(248, 81, 73));
        lcd.setCursor(8, 24);
        lcd.printf("Master: %s", st.online ? "ON" : "OFF");

        // 运行时间
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        lcd.setCursor(8, 40);
        unsigned long up = nowSec;
        lcd.printf("Uptime: %lum%lus", up / 60, up % 60);

        // 雷达累计字节
        lcd.setCursor(8, 56);
        lcd.printf("Radar bytes: %lu", st.rx_bytes);

        // 转向状态
        const char *turnTxt[] = {"OFF", "LEFT", "RIGHT"};
        lcd.setCursor(8, 72);
        lcd.printf("Turn: %s", turnTxt[st.turn % 3]);

        // 蜂鸣模式
        const char *bzTxt[] = {"Mute", "BSD beep", "RCW 4Hz", "Turn"};
        lcd.setTextColor(st.bz_mode > 0 ? lgfx::color888(248, 81, 73) : lgfx::color888(139, 148, 158));
        lcd.setCursor(8, 88);
        lcd.printf("Buzzer: %s", bzTxt[st.bz_mode % 4]);

        // 后方预警
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        lcd.setCursor(8, 104);
        lcd.printf("L:%s R:%s", st.rcw_l ? "RCW" : " - ", st.rcw_r ? "RCW" : " - ");

        // 目标列表
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setCursor(8, 124);
        lcd.printf("Targets (%d):", st.obj_num);
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            const TermObj &t = st.objs[i];
            lcd.setCursor(16, 140 + i * 14);
            lcd.printf("t%d: %dm %d' %dm/s", i, t.range, t.angle, t.velocity);
        }

        // 底部提示
        lcd.setTextColor(lgfx::color888(139, 148, 158));
        lcd.setCursor(8, lcd.height() - 12);
        lcd.print("< > page");

        // 顶部翻页按钮 ‹ ›
        drawNavBtn(lcd, false);
        drawNavBtn(lcd, true);

        lcd.endWrite();
    }
};
