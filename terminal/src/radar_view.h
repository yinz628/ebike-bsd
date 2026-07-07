// ============================================================
//  radar_view.h - 雷达扇形可视化 (Page 0, 主页)
//  本车 🚲 在顶部中央, 目标按角度+距离画在扇形内
//  数学移植自 firmware/wifi_web.h 的 Canvas JS 实现
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"

extern LGFX lcd;

class RadarView {
public:
    void draw(const TerminalState &st) {
        int w = lcd.width();
        int h = lcd.height();
        // 本车位置: 顶部中央, 扇形向下展开
        int cx = w / 2;
        int cy = 28;
        int maxR = h - cy - 14;
        int detRange = 25;   // 最大显示距离 (m), 与主控 config.rcw.range_limit 对齐

        // 清屏 (深色背景)
        lcd.fillScreen(lgfx::color888(13, 17, 23));

        const float toRad = PI / 180.0f;
        const float base  = PI / 2;       // 扇形朝下
        const float FOV   = 80;           // ±40°
        const float half  = FOV / 2 * toRad;

        // 扇形背景
        lcd.fillArc(cx, cy, maxR, 0, base - half, base + half, lgfx::color888(22, 27, 34));

        // 距离弧线 (25/50/75/100%)
        lcd.setTextColor(lgfx::color888(139, 148, 158), lgfx::color888(13, 17, 23));
        lcd.setTextSize(1);
        for (int i = 1; i <= 4; i++) {
            int dm = detRange * i / 4;
            int r = (int)((float)dm / detRange * maxR);
            lcd.drawArc(cx, cy, r, 0, base - half, base + half, lgfx::color888(48, 54, 61));
            // 距离标注 (左下角)
            lcd.setCursor(cx + (int)(r * cosf(base + half)) - 18,
                          cy + (int)(r * sinf(base + half)) + 4);
            lcd.printf("%dm", dm);
        }

        // ±40° 边界虚线
        lcd.drawLine(cx, cy,
                     cx + (int)(maxR * cosf(base - half)),
                     cy + (int)(maxR * sinf(base - half)),
                     lgfx::color888(88, 166, 255));
        lcd.drawLine(cx, cy,
                     cx + (int)(maxR * cosf(base + half)),
                     cy + (int)(maxR * sinf(base + half)),
                     lgfx::color888(88, 166, 255));

        // 目标红点
        uint16_t red    = lgfx::color888(248, 81, 73);
        uint16_t redDim = lgfx::color888(248, 81, 73, 80);
        uint16_t txtCol = lgfx::color888(201, 209, 217);
        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            const TermObj &t = st.objs[i];
            float ar = t.angle * toRad;
            float r = (float)t.range / detRange * maxR;
            // 注意: 主控视角本车朝上(扇形朝下), 角度负=左=屏幕右侧? 不,
            // 主控坐标: 负=左后方, 正=右后方. 屏幕映射: 左后方→屏幕左, 右后方→屏幕右.
            // base - angle (左负往左偏, 右正往右偏): 用 base - angle 让负角往左
            int tx = cx + (int)(r * cosf(base - ar));
            int ty = cy + (int)(r * sinf(base - ar));
            // 限制在扇形内
            tx = constrain(tx, 4, w - 4);
            ty = constrain(ty, cy, cy + maxR);

            // 外圈光晕 + 实心点
            lcd.fillCircle(tx, ty, 7, redDim);
            lcd.fillCircle(tx, ty, 3, red);

            // 标注: "角度° 距离m"
            lcd.setTextColor(txtCol, lgfx::color888(13, 17, 23));
            lcd.setTextDatum(t.angle >= 0 ? TL_DATUM : TR_DATUM);
            lcd.setCursor(tx + (t.angle >= 0 ? 6 : -6), ty - 10);
            lcd.printf("%d° %dm", t.angle, t.range);
            if (t.velocity != 0) {
                lcd.printf(" %dm/s", t.velocity);
            }
        }
        lcd.setTextDatum(TL_DATUM);

        // 本车图标 🚲 (顶部中央)
        lcd.setTextColor(lgfx::color888(88, 166, 255), lgfx::color888(13, 17, 23));
        lcd.setTextSize(2);
        lcd.setCursor(cx - 8, cy - 18);
        lcd.print("^^");   // ASCII 近似 (无 emoji 字库)
        lcd.setTextSize(1);

        // 底部状态条
        drawStatusBar(0, h - 14, w, 14, st);
    }

private:
    void drawStatusBar(int x, int y, int w, int h, const TerminalState &st) {
        lcd.fillRect(x, y, w, h, lgfx::color888(22, 27, 34));

        // 左/右指示灯状态点
        uint16_t off = lgfx::color888(48, 54, 61);
        uint16_t warn = lgfx::color888(210, 153, 34);    // 黄 (BSD 慢闪/转向)
        uint16_t danger = lgfx::color888(248, 81, 73);   // 红 (RCW 快闪)

        // 左灯 (ind_l: 0灭 1BSD 2RCW 3转向)
        uint16_t lc = (st.ind_l == 2) ? danger : (st.ind_l > 0 ? warn : off);
        lcd.fillCircle(x + 10, y + h/2, 4, lc);
        // 右灯
        uint16_t rc = (st.ind_r == 2) ? danger : (st.ind_r > 0 ? warn : off);
        lcd.fillCircle(x + 24, y + h/2, 4, rc);

        // 文字: 目标数 + 蜂鸣 + 转向
        lcd.setTextColor(lgfx::color888(201, 209, 217), lgfx::color888(22, 27, 34));
        lcd.setCursor(x + 36, y + 3);
        lcd.printf("%dtgt", st.obj_num);

        const char *bzTxt[] = {"", "BEEP", "4Hz", "ALARM"};
        if (st.bz_mode > 0 && st.bz_mode <= 3) {
            lcd.setTextColor(lgfx::color888(248, 81, 73), lgfx::color888(22, 27, 34));
            lcd.printf(" BZ:%s", bzTxt[st.bz_mode]);
        }

        if (st.turn == 1 || st.turn == 2) {
            lcd.setTextColor(lgfx::color888(88, 166, 255), lgfx::color888(22, 27, 34));
            lcd.printf(" %s", st.turn == 1 ? "LEFT<<<" : ">>>RIGHT");
        }

        // 右侧: 连接状态
        lcd.setTextColor(st.online ? lgfx::color888(63, 185, 80) : lgfx::color888(248, 81, 73),
                         lgfx::color888(22, 27, 34));
        lcd.setCursor(w - 50, y + 3);
        lcd.print(st.online ? "ONLINE" : "OFFLINE");
    }
};
