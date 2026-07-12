// ============================================================
//  theme.h - C3 终端主题色集中定义
//
//  消除 4 个视图文件 + c3_terminal.ino 里 76 处 color888 魔法数字重复.
//  换主题色只需改这里一处.
//
//  用法: lcd.setTextColor(theme::FG, theme::BG);
// ============================================================
#ifndef C3_THEME_H
#define C3_THEME_H

#include <LovyanGFX.hpp>

namespace theme {
    // 背景层
    constexpr uint32_t BG       = lgfx::color888(13, 17, 23);     // 主背景 (深蓝黑)
    constexpr uint32_t BG_PANEL = lgfx::color888(22, 27, 34);     // 面板/状态条底
    constexpr uint32_t BG_DIM   = lgfx::color888(48, 54, 61);     // 禁用/off/分隔线

    // 文字
    constexpr uint32_t FG       = lgfx::color888(201, 209, 217);  // 正文 (浅灰白)
    constexpr uint32_t FG_INV   = lgfx::color888(0, 0, 0);        // 反色文字 (亮底上)
    constexpr uint32_t WHITE    = lgfx::color888(255, 255, 255);  // 纯白 (强调)
    constexpr uint32_t MUTED    = lgfx::color888(139, 148, 158);  // 次要文字 (灰)

    // 语义色
    constexpr uint32_t ACCENT   = lgfx::color888(88, 166, 255);   // 蓝 (主色/标题/边界)
    constexpr uint32_t OK       = lgfx::color888(63, 185, 80);    // 绿 (online/plus/SAVE)
    constexpr uint32_t DANGER   = lgfx::color888(248, 81, 73);    // 红 (RCW/offline/minus)
    constexpr uint32_t WARN     = lgfx::color888(210, 153, 34);   // 黄 (BSD 慢闪/未保存)

    // 派生色
    constexpr uint32_t RED_DIM  = lgfx::color565(180, 50, 45);    // 红色光晕 (半透明感)
}

#endif // C3_THEME_H
