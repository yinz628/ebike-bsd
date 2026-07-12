// ============================================================
//  types.h - 跨文件共享的类型定义
//
//  解决 TurnState_t 原本定义在 ebike_bsd.ino 里、靠 include 顺序
//  才能被 wifi_web.h/terminal_link.h 的 extern 引用到的问题.
//  移到这里后, 任何头文件 #include "types.h" 即可使用 TurnState_t,
//  不再依赖 .ino 的 include 顺序.
// ============================================================
#ifndef EBIKE_TYPES_H
#define EBIKE_TYPES_H

// 转向灯状态 (三档自锁开关: LEFT=LOW / RIGHT=LOW / 中间=OFF)
typedef enum {
    TURN_OFF = 0,
    TURN_LEFT,
    TURN_RIGHT
} TurnState_t;

#endif // EBIKE_TYPES_H
