# ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)

把立创实战派 ESP32-C3 开发板作为 ebike-bsd 系统的可视化终端,通过 UART 有线连接车尾主控,显示雷达目标可视化、系统状态,并支持触摸配置参数。

## 架构

```
车尾主控 ESP32-32D                    车把终端 实战派 ESP32-C3
  UART1 (GPIO27=TX, GPIO32=RX)  ←→   GPIO18/19 多功能口 (复用为 UART)
  5V / GND  ──────────────────────→   5V / GND (供电)
  每 100ms 推送 $S 状态帧              接收 → ST7789 显示 + ES8311 报警音
  接收 $C 命令 → 改 config             触摸 → 发送 $C 命令
```

一根 4 芯线(CH1.25 接口)同时承载供电和双向通信,延迟 <6ms。

## 接线

### 主控侧 (ESP32-32D) → 用排针引出的空闲 GPIO

| 主控 GPIO | 方向 | 连接到 | C3 多功能口 |
|:-:|---|---|:-:|
| GPIO27 (TX) | → | C3 RX | IO18 |
| GPIO32 (RX) | ← | C3 TX | IO19 |
| 5V | → | C3 5V | 5V |
| GND | ↔ | C3 GND | GND |

> ⚠️ 主控 GPIO27 原为双闪按钮位(V2.6 已移除),GPIO32 在 38pin 板右侧排针。若你的板子上这两个脚没引出,可改 `terminal_link.h` 的 `TERM_TX_PIN/TERM_RX_PIN`。

### C3 侧 (实战派)

多功能口(1号 CH1.25)的 IO18/IO19 默认是 USB D-/D+,但**实战派板载独立的 USB-TTL 芯片**(走 Type-C 烧录),所以复用 GPIO18/19 不影响 USB 烧录。

## 烧录

### C3 终端
```bash
cd terminal
pio run -t upload    # USB-C 数据线接 Type-C 口, 正常烧录
```
C3 的烧录完全走板载 USB-TTL,与 GPIO18/19 复用无关。

### 主控 (改动后需重新烧录)
```bash
cd firmware
pio run -t upload
```

## UART 协议 (ASCII 文本帧, 115200bps)

### 主控 → C3: 状态帧 `$S`
```
$S,<obj_num>,<bz_mode>,<rcw_l>,<rcw_r>,<ind_l>,<ind_r>,<turn>,<rx_bytes>,<valid>,<t1_range>,<t1_angle>,<t1_velo>,<t1_id>,...\n
```
示例: `$S,1,2,1,0,2,0,1,12345,1,8,-28,4,1\n`

| 字段 | 含义 |
|---|---|
| obj_num | 目标数 (0..4) |
| bz_mode | 蜂鸣模式 0静音/1BSD短鸣/2RCW4Hz/3转向长鸣 |
| rcw_l/r | 左/右后碰撞预警 (0/1) |
| ind_l/r | 左/右指示灯模式 0灭/1BSD慢闪/2RCW快闪/3转向常亮 |
| turn | 转向 0off/1left/2right |
| rx_bytes | 主控雷达累计字节 (诊断) |
| valid | 雷达帧有效 (0/1) |
| t*_range/angle/velo/id | 各目标 距离m/角度°/速度m·s/ID |

### C3 → 主控: 命令帧 `$C`
```
$C,<key>=<value>\n     例: $C,sensitivity=2\n
$C,SAVE\n              保存配置到 NVS
$C,RESET\n             出厂重置
```

可配置 key: `rcw_speed, rcw_low, rcw_range, rcw_hold, rcw_lmin/lmax/rmin/rmax, turn_speed, turn_range, sensitivity, det_range, beep_cool`

## 界面 (3 页, 触摸顶部/底部切页)

| 页 | 内容 |
|---|---|
| Page 0 雷达图 | 扇形可视化, 红点目标, 底部状态条 |
| Page 1 状态 | 连接/运行时间/雷达字节/转向/蜂鸣/目标列表 |
| Page 2 配置 | 5 个参数 [</>] 调节 + [保存][出厂重置] |

## 分阶段验证

本工程用条件编译开关(`c3_terminal.ino` 顶部)控制各功能模块,便于逐阶段验证:

```cpp
#define ENABLE_RADAR_VIEW     // P2: 雷达扇形图 (默认开)
// #define ENABLE_STATUS_VIEW    // P3: 状态页
// #define ENABLE_CONFIG_VIEW    // P3: 配置页 + 触摸
// #define ENABLE_ALERT_SOUND    // P4: ES8311 报警音
```

| 阶段 | 开关 | 验证 |
|:-:|---|---|
| P1 | 全关 | 串口看到 `[LINK] obj=N ...` 解析日志 |
| P2 | RADAR_VIEW | 屏幕显示扇形图, 手挥动时红点跟随 |
| P3 | +STATUS/+CONFIG | 触摸切页, <> 调参后主控串口确认收到 |
| P4 | +ALERT_SOUND | 主控蜂鸣响时 C3 扬声器同步 |

## 待补全 (P4)

`alert_sound.h` 的 `initES8311Basic()` 留有 TODO:ES8311 的 I2C 寄存器初始化序列需参考实战派 C3 示例补全。I2S 引脚(WS/BCK/DOUT)需按实战派原理图核对(I2S_WS=7/I2S_BCK=10/I2S_DOUT=11 是常见值,实施时确认)。

## 主控固件改动

仅 3 处,纯增量,不影响现有功能:
1. `firmware/terminal_link.h` — 新增 UART 协议封装
2. `firmware/ebike_bsd.ino` setup() — 加 `terminalLinkInit()`
3. `firmware/ebike_bsd.ino` loop() — 加 `terminalLinkUpdate()`(在 updateBuzzer 之后)

现有 WiFi Web 控制台、雷达解析、BSD/RCW 逻辑全部不动。
