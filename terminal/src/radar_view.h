// ============================================================
//  radar_view.h - 雷达扇形可视化 (Page 0, 主页)
//  本车 🚲 在顶部中央, 目标按角度+距离画在扇形内
//  数学移植自 firmware/wifi_web.h 的 Canvas JS 实现
//
//  防闪屏: 静态背景(扇形/弧线/标注/边界/本车)预渲染到离屏 Sprite,
//          每帧 pushSprite 一次性覆盖旧目标点(无黑屏间隙), 再画动态目标.
//          整帧绘制用 startWrite/endWrite 包裹, 避免 SPI 分段传输撕裂.
// ============================================================
#pragma once
#include "lgfx_config.hpp"
#include "uart_link.h"

extern LGFX lcd;

class RadarView {
private:
    LGFX_Sprite _bg;          // 静态背景缓存 (含扇形/标注/本车/翻页按钮)
    bool _bg_ready = false;   // 背景是否已渲染
    int _last_w = 0, _last_h = 0;

    // 扇形几何参数 (缓存, 避免重复计算)
    int _cx = 0, _cy = 0, _maxR = 0;
    static const int _detRange = 40;   // 最大显示距离 (m), 与主控 config.rcw.range_limit 对齐

    // 防闪屏: 脏标志 + 目标摘要. 仅当目标/状态变化或切页时重绘, 触摸不触发重绘.
    bool _dirty = true;
    // 目标摘要 (4个目标 × 3字段, 打包比较)
    int8_t  _sum_tgt[12] = {0};
    uint8_t _sum_obj = 0xFF, _sum_bz = 0xFF, _sum_indl = 0xFF, _sum_indr = 0xFF, _sum_turn = 0xFF;
    bool    _sum_online = false;

    bool summaryChanged(const TerminalState &st) {
        if (st.obj_num != _sum_obj || st.bz_mode != _sum_bz || st.ind_l != _sum_indl
            || st.ind_r != _sum_indr || st.turn != _sum_turn || st.online != _sum_online)
            return true;
        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            int b = i * 3;
            if (st.objs[i].range    != _sum_tgt[b])   return true;
            if (st.objs[i].angle    != _sum_tgt[b+1]) return true;
            if (st.objs[i].velocity != _sum_tgt[b+2]) return true;
        }
        return false;
    }
    void saveSummary(const TerminalState &st) {
        _sum_obj = st.obj_num; _sum_bz = st.bz_mode; _sum_indl = st.ind_l;
        _sum_indr = st.ind_r; _sum_turn = st.turn; _sum_online = st.online;
        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            int b = i * 3;
            _sum_tgt[b]   = st.objs[i].range;
            _sum_tgt[b+1] = st.objs[i].angle;
            _sum_tgt[b+2] = st.objs[i].velocity;
        }
    }

    // 预渲染静态背景到 Sprite
    void buildBackground() {
        int w = lcd.width();
        int h = lcd.height();
        _cx = w / 2;
        _cy = 28;
        _maxR = h - _cy - 14;

        _bg.createSprite(w, h);
        _bg.fillScreen(lgfx::color888(13, 17, 23));

        // ⚠ LovyanGFX 的 fillArc/drawArc 角度参数是度数(deg), 内部自转弧度.
        //   而 cosf/sinf 需要 弧度(rad). 两者必须分开, 不可混用!
        const float toRad = PI / 180.0f;
        const int   baseDeg = 90;          // 扇形朝下 (屏幕坐标 90°=向下)
        const int   FOV     = 80;          // ±40°
        const int   a0      = baseDeg - FOV/2;   // 50° 左边界
        const int   a1      = baseDeg + FOV/2;   // 130° 右边界
        const float a0Rad   = a0 * toRad;
        const float a1Rad   = a1 * toRad;
        const float baseRad = baseDeg * toRad;

        // 扇形背景填充 (度数!)
        _bg.fillArc(_cx, _cy, _maxR, 0, a0, a1, lgfx::color888(22, 27, 34));

        // 距离弧线 (10/20/30/40m, 每 10m 一条)
        // drawArc(r0=r, r1=r-1) 画 1px 细弧带, 避免画到圆心的半径线
        _bg.setTextColor(lgfx::color888(139, 148, 158), lgfx::color888(13, 17, 23));
        _bg.setTextSize(1);
        uint32_t arcCol = lgfx::color888(48, 54, 61);
        for (int dm = 10; dm <= _detRange; dm += 10) {
            int r = (int)((float)dm / _detRange * _maxR);
            _bg.drawArc(_cx, _cy, r, r > 1 ? r - 1 : 0, a0, a1, arcCol);
            // 标注用弧度算坐标
            _bg.setCursor(_cx + (int)(r * cosf(a1Rad)) - 18,
                          _cy + (int)(r * sinf(a1Rad)) + 4);
            _bg.printf("%dm", dm);
        }

        // ±40° 边界线 (用弧度算端点)
        _bg.drawLine(_cx, _cy,
                     _cx + (int)(_maxR * cosf(a0Rad)),
                     _cy + (int)(_maxR * sinf(a0Rad)),
                     lgfx::color888(88, 166, 255));
        _bg.drawLine(_cx, _cy,
                     _cx + (int)(_maxR * cosf(a1Rad)),
                     _cy + (int)(_maxR * sinf(a1Rad)),
                     lgfx::color888(88, 166, 255));

        // 本车图标 ^^ (顶部中央, 蓝色)
        _bg.setTextColor(lgfx::color888(88, 166, 255), lgfx::color888(13, 17, 23));
        _bg.setTextSize(2);
        _bg.setCursor(_cx - 8, _cy - 18);
        _bg.print("^^");

        // 顶部翻页按钮 ‹ › (固定在左右上角)
        drawNavBtn(_bg, false);
        drawNavBtn(_bg, true);

        _bg_ready = true;
        _last_w = w;
        _last_h = h;
    }

public:
    void markDirty() { _dirty = true; }   // 切页时调用, 强制重绘

    void draw(const TerminalState &st) {
        int w = lcd.width();
        int h = lcd.height();

        // 分辨率变化或首次绘制 → 重建背景
        if (!_bg_ready || w != _last_w || h != _last_h) {
            buildBackground();
        }

        // 防闪屏: 仅当目标/状态变化或切页时重绘 (触摸不触发, demo 数据静态则不闪)
        if (!_dirty && !summaryChanged(st)) return;
        _dirty = false;
        saveSummary(st);

        lcd.startWrite();       // 整帧原子提交, 避免 SPI 分段撕裂

        // 1) 一次性推背景 (覆盖旧目标点和旧文字, 无黑屏间隙)
        _bg.pushSprite(&lcd, 0, 0);

        // 2) 画动态目标红点 (直接画在屏幕上, 下一帧会被背景覆盖)
        const float toRad = PI / 180.0f;
        const float base  = PI / 2;
        uint32_t red    = lgfx::color888(248, 81, 73);
        uint32_t redDim = lgfx::color565(180, 50, 45);
        uint32_t txtCol = lgfx::color888(201, 209, 217);

        int n = min((int)st.obj_num, 4);
        for (int i = 0; i < n; i++) {
            const TermObj &t = st.objs[i];
            float ar = t.angle * toRad;
            float r = (float)t.range / _detRange * _maxR;
            int tx = _cx + (int)(r * cosf(base - ar));
            int ty = _cy + (int)(r * sinf(base - ar));
            tx = constrain(tx, 4, w - 4);
            ty = constrain(ty, _cy, _cy + _maxR);

            // 外圈光晕 + 实心点
            lcd.fillCircle(tx, ty, 7, redDim);
            lcd.fillCircle(tx, ty, 3, red);

            // 标注: 只显示速度 (距离和角度可从图中位置读出)
            lcd.setTextColor(txtCol, lgfx::color888(13, 17, 23));
            lcd.setTextDatum(t.angle >= 0 ? TL_DATUM : TR_DATUM);
            lcd.setCursor(tx + (t.angle >= 0 ? 6 : -6), ty - 10);
            lcd.printf("%dm/s", t.velocity);
        }
        lcd.setTextDatum(TL_DATUM);

        // 3) 底部状态条 (画在背景之上, 每帧刷新)
        drawStatusBar(0, h - 14, w, 14, st);

        lcd.endWrite();
    }

private:
    void drawStatusBar(int x, int y, int w, int h, const TerminalState &st) {
        lcd.fillRect(x, y, w, h, lgfx::color888(22, 27, 34));

        // 左/右指示灯状态点
        uint32_t off    = lgfx::color888(48, 54, 61);
        uint32_t warn   = lgfx::color888(210, 153, 34);    // 黄 (BSD 慢闪/转向)
        uint32_t danger = lgfx::color888(248, 81, 73);     // 红 (RCW 快闪)

        uint32_t lc = (st.ind_l == 2) ? danger : (st.ind_l > 0 ? warn : off);
        lcd.fillCircle(x + 10, y + h/2, 4, lc);
        uint32_t rc = (st.ind_r == 2) ? danger : (st.ind_r > 0 ? warn : off);
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

        // 右侧: 连接状态 + 版本号
        lcd.setTextColor(st.online ? lgfx::color888(63, 185, 80) : lgfx::color888(248, 81, 73),
                         lgfx::color888(22, 27, 34));
        lcd.setCursor(w - 50, y + 3);
        lcd.print(st.online ? "ONLINE" : "OFFLINE");
        // 固件版本号 (与主控 FW_VERSION 一致, 右侧下方显示)
        lcd.setTextColor(lgfx::color888(139, 148, 158), lgfx::color888(22, 27, 34));
        lcd.setCursor(w - 50, y + 12);
        lcd.print(FW_VERSION);
    }
};
