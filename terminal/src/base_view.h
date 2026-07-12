// ============================================================
//  base_view.h - C3 视图抽象基类
//
//  解决"加页面是散弹枪修改"问题: 三个视图原本各自独立无基类,
//  c3_terminal.ino 用 switch(currentPage) 分发, 加一页要改 5 处.
//
//  继承 BaseView 后, c3_terminal.ino 用 pages[] 数组 + 虚函数分发,
//  加一页只需数组加一个元素.
// ============================================================
#ifndef C3_BASE_VIEW_H
#define C3_BASE_VIEW_H

#include "uart_link.h"   // TerminalState

// 视图抽象基类: 所有页面实现这 4 个接口 (后两个有默认空实现)
class BaseView {
public:
    virtual ~BaseView() {}

    // 绘制当前页 (主循环每帧调用, 内部应做脏检测避免闪屏)
    virtual void draw(const TerminalState &st) = 0;

    // 标记需要重绘 (切页时调用)
    virtual void markDirty() = 0;

    // 进入该页时调用 (默认空; ConfigView 覆盖以同步主控配置)
    virtual void onEnter(const TerminalState &st) { (void)st; }

    // 触摸事件 (默认无触摸; ConfigView 覆盖以处理参数调整)
    // 返回 true 表示事件已处理, false 表示未命中
    virtual bool handleTouch(int tx, int ty) { (void)tx; (void)ty; return false; }
};

#endif // C3_BASE_VIEW_H
