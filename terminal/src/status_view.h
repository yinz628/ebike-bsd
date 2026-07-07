// ============================================================
//  status_view.h - 系统状态页 (Page 1)
//  显示: 连接状态/运行时间/雷达字节/转向/蜂鸣/各目标详情
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"

extern LGFX lcd;

class StatusView {
private:
    unsigned long bootMs;
public:
    StatusView() : bootMs(0) {}

    void draw(const TerminalState &st) {
        if (bootMs == 0) bootMs = millis();

        lcd.fillScreen(lgfx::color888(13, 17, 23));

        // 标题
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setTextSize(1);
        lcd.setCursor(8, 6);
        lcd.print("[ 系统状态 ]");

        // 连接状态
        lcd.setTextColor(st.online ? lgfx::color888(63, 185, 80) : lgfx::color888(248, 81, 73));
        lcd.setCursor(8, 24);
        lcd.printf("主控: %s", st.online ? "● 在线" : "○ 离线");

        // 运行时间
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        lcd.setCursor(8, 40);
        unsigned long up = (millis() - bootMs) / 1000;
        lcd.printf("运行: %lum%lus", up / 60, up % 60);

        // 雷达累计字节
        lcd.setCursor(8, 56);
        lcd.printf("雷达字节: %lu", st.rx_bytes);

        // 转向状态
        const char *turnTxt[] = {"关闭", "左转", "右转"};
        lcd.setCursor(8, 72);
        lcd.printf("转向: %s", turnTxt[st.turn % 3]);

        // 蜂鸣模式
        const char *bzTxt[] = {"静音", "BSD短鸣", "RCW 4Hz", "转向长鸣"};
        lcd.setTextColor(st.bz_mode > 0 ? lgfx::color888(248, 81, 73) : lgfx::color888(139, 148, 158));
        lcd.setCursor(8, 88);
        lcd.printf("蜂鸣: %s", bzTxt[st.bz_mode % 4]);

        // 后方预警
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        lcd.setCursor(8, 104);
        lcd.printf("左RCW:%s 右RCW:%s", st.rcw_l ? "是" : "否", st.rcw_r ? "是" : "否");

        // 目标列表
        lcd.setTextColor(lgfx::color888(88, 166, 255));
        lcd.setCursor(8, 124);
        lcd.printf("目标 (%d):", st.obj_num);
        lcd.setTextColor(lgfx::color888(201, 209, 217));
        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            const TermObj &t = st.objs[i];
            lcd.setCursor(16, 140 + i * 14);
            lcd.printf("t%d: %dm %d° %dm/s", i, t.range, t.angle, t.velocity);
        }

        // 底部提示
        lcd.setTextColor(lgfx::color888(139, 148, 158));
        lcd.setCursor(8, lcd.height() - 12);
        lcd.print("顶部=上一页 底部=下一页");
    }
};
