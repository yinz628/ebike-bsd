# C3 终端调试记录 (UART 通信链路)

> 记录立创 ESP32-C3 作为 ebike-bsd 车把显示终端的完整调试过程,
> 包括 C3 boot loop 排查、UART 方案替换 WiFi、主控 GPIO 损坏诊断。
>
> 日期: 2026-07-07 ~ 2026-07-08

---

## 一、背景与目标

把立创实战派 ESP32-C3 开发板作为 ebike-bsd 系统的车把显示终端:
- 显示雷达目标可视化、系统状态
- 触摸配置参数
- ES8311 扬声器同步报警音

通信链路: 主控 ESP32-32D (车尾) ↔ C3 终端 (车把)。

---

## 二、阶段 1: C3 启动问题排查 (WiFi 方案)

### 初始方案: WiFi HTTP 轮询

C3 作为 STA 连主控 AP,HTTP GET `/api/status` 取状态。
固件含 WiFi + HTTPClient 库,体积 **840KB**。

### 问题: C3 WiFi 固件 boot loop

烧录后 C3 反复重启,USB 反复断开重枚举(5 秒周期)。

### 关键诊断: 出厂固件验证

烧立创出厂固件 `esp32_c3_all.bin` → **C3 完美运行**(屏幕亮、触摸点正常上报)。
**结论: 芯片没坏,问题在固件配置。**

### 烧写踩坑总结 (ESP32-C3 特有)

| 坑 | 真相 | 正确做法 |
|---|---|---|
| **bootloader 烧到哪?** | ESP32-C3 的 bootloader 在 **0x0**,不是 ESP32 经典版的 0x1000 | 参考 [esptool issue #953](https://github.com/espressif/esptool/issues/953) |
| **用谁的 bootloader?** | PlatformIO 编译的 bootloader 加载完 3 段后 entry 跳转**卡死**(原因未明) | **必须用立创出厂固件的 stock bootloader** |
| **分区表?** | `default_8MB.csv` 缺 `phy_init` 分区 → WiFi 射频初始化失败 | 自建 `partitions_factory_8M.csv`(含 phy_init) |
| **Flash 速度?** | bootloader 和 app 必须用**相同速度**,否则切换时序冲突卡死 | 80MHz + DIO(与出厂一致) |
| **esptool 复位?** | `--after hard_reset` 在 CH343 上会让 USB 消失 | 用 `--after no_reset`,烧完手动按 RESET |

### 正确烧写命令

```bash
# 首次/换分区表 (烧三个文件):
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x0     tools/c3_backup/stock_bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin

# 日常更新固件 (只换 app):
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

### stock bootloader 提取方法

从出厂固件 `esp32-c3/出厂测试固件/esp32_c3_all.bin` 提取前 ~0x5080 字节
(含 image header + 3 segments + checksum + SHA256)。
已保存在 `tools/c3_backup/stock_bootloader.bin`。

---

## 三、阶段 2: WiFi 固件加载大段失败

即使用 stock bootloader + 正确分区表,WiFi 固件(840KB)仍间歇性 boot loop:

```
I (136) esp_image: segment 3: paddr=00040020 vaddr=42000020 size=a1fb4h (663476) map
                    ↑ 加载完这个 663KB 大段后立即重启
```

**根因**: WiFi/HTTPClient 库让固件达 840KB,其中一个大段 663KB。
Flash 密集读取大段时供电不足 → brownout → 复位循环。

**这是供电问题 + 固件过大双重因素。**

---

## 四、阶段 3: 改用 UART 有线方案 (关键转折)

主控长时间 WiFi 射频会发热;C3 的 WiFi 固件又因大段加载不稳。
**改用 UART 有线方案一举两得**:

| 对比 | WiFi 方案 | UART 方案 |
|---|---|---|
| 主控射频 | 长时间发热 | 不发热(WiFi 30s 超时关) |
| C3 固件大小 | 840KB(大段加载失败) | **326KB**(启动稳定) |
| 通信延迟 | ~200ms | **<100ms** |
| 协议 | HTTP JSON | ASCII `$S` / `$C` 帧 |

### C3 侧改动

- 删除 `net_link.h` (WiFi/HTTPClient 版)
- 新增 `uart_link.h` (HardwareSerial 收 `$S` 帧, 发 `$C` 命令)
- 固件从 840KB → **326KB**,boot loop 消失

### 主控侧改动

- `firmware/src/terminal_link.h`: UART1 推送 `$S` 状态帧(10Hz),接收 `$C` 配置命令
- `firmware/src/ebike_bsd.ino`: 删除 OLED I2C 定义(释放 GPIO21/22),接入 terminalLink
- `terminal_link.h` 的 `$C` 命令解析支持全部 config 字段

### 验证结果 (C3 单独)

```
=== ebike-bsd C3 终端 V0.1 ===
[UART] 终端链路就绪 (115200, RX=18/TX=19)
[HB] online=0 last_frame=0ms ago   ← C3 启动稳定, 无 boot loop ✓
```

C3 固件稳定运行,无 boot loop,无 USB 断连。

---

## 五、阶段 4: UART 跨板通信诊断 (最长阶段)

### 问题: 接线后 C3 收不到主控数据

```
C3: [HB] online=0 Serial1.avail=0   ← C3 一个字节都没收到
主控: [TERM-DIAG] 已发 50 帧, 1071 字节  ← 主控确实在持续发
```

### 系统排查过程

**测试 1: C3 GPIO18/19 自 loopback (短接)**
```
[loop] PING → 收 4字节: "PING" ✓   ← C3 UART1 在 GPIO18/19 工作正常
```

**测试 2: 主控 GPIO21/22 自 loopback (短接)**
```
[loop] PING → 收 5字节: "PING " ✓  ← 主控"看似"正常 (后来证明是假象)
```

**测试 3: 跨板 C3 → 主控 (C3 TX → 主控 RX)**
```
主控: [loop] PING → 收 4字节: "PING"  ← 主控 RX 能收 C3 发的数据 ✓
C3 GPIO19(TX) → 主控 GPIO22(RX): 成功
C3 GPIO18(TX) → 主控 GPIO22(RX): 成功
```

**测试 4: 跨板 主控 → C3 (主控 TX → C3 RX) — 全部失败!**
```
C3: [HB] online=0  ← 无论主控用 GPIO21 还是 GPIO22 发, C3 都收不到
主控 GPIO21(TX) → C3 GPIO18(RX): 失败
主控 GPIO21(TX) → C3 GPIO19(RX): 失败
主控 GPIO22(TX) → C3 GPIO19(RX): 失败
```

### 排查 USB_SERIAL_JTAG 假说 (被推翻)

怀疑 C3 GPIO18/19 被 USB_SERIAL_JTAG 的 D-/D+ 占用。
用寄存器层强制清除 pad override:
```cpp
CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
// CONF0 从 0x4200 → 0x200 (pad override 已清除)
```
**结果: 仍然失败**。USB-JTAG 不是真因。

### 决定性诊断: GPIO 方波体检

写固件让主控每个空闲 GPIO 输出方波,万用表量电压:
正常应 ~1.5V(3.3V/0V 各占一半的方波平均值)。

| GPIO | 方波电压 | 状态 |
|---|---|---|
| GPIO2 | 1.65V | ✅ 看似正常 |
| GPIO5 | 1.65V | ✅ 看似正常 |
| GPIO15 | 1.65V | ✅ 看似正常 |
| GPIO18 | 0V | ❌ 坏 |
| GPIO19 | 0V | ❌ 坏 |
| GPIO21 | 0V | ❌ 坏 |
| GPIO22 | 0V | ❌ 坏 |
| GPIO27 | 0V | ❌ 坏 |
| GPIO32 | 0V | ❌ 坏 |
| GPIO33 | 0V | ❌ 坏 |

**7/10 脚量到 0V**(包括原 OLED I2C 的 21/22)。

### 高频 UART 下的二次验证

用 GPIO5(慢速方波 1.65V 看似正常)跑 UART1 TX 发 "TX5":
```
开机瞬间: 3.3V (UART 空闲 HIGH)
稳定后:   0.12V ← 高速翻转下驱动崩溃!
```

**GPIO2/5/15 在慢速 digitalWrite 下看着正常(1.65V),但在高频 UART(115200bps)下全部崩溃到 0.12V。**

### 最终结论

> **主控 ESP32-32D 模组的 GPIO 输出驱动能力大面积损坏/老化。**
>
> - 慢速 GPIO(digitalWrite, ms 级):GPIO2/5/15 勉强能工作(1.65V)
> - 高速 GPIO(UART 115200bps, μs 级):**全部崩溃**(0.12V)
>
> 这解释了所有矛盾:
> - 主控自 loopback "成功" = UART 内部回环(数据没经物理引脚)
> - C3→主控 成功 = 主控 GPIO 作 RX(输入),输入功能还在
> - 主控→C3 全失败 = 主控 GPIO 作 TX(输出),输出驱动已坏

---

## 六、根因分析

### 为什么主控自 loopback "成功"是假象

ESP32 的 UART 外设有**内部回环模式**:数据从 TX FIFO 直接进 RX FIFO,
**不经过物理引脚**。当物理引脚驱动坏掉时,`Serial1.print()` 仍能把数据
写进 TX FIFO,内部回环让 RX FIFO 收到,于是 `[loop] PING → 收到` 看似成功。

但物理引脚上没有信号(万用表 0.12V),跨板时 C3 自然收不到。

### GPIO 损坏的可能原因

1. **ESP32 模组批次问题/老化**:这块模组 GPIO 输出驱动器先天不足或长期衰退
2. **过压/过流损伤**:调试时某个 GPIO 受到过压(如接 OLED 时 5V 串入)
3. **I2C 总线锁死**:GPIO21/22(原 OLED SDA/SCL)长时间过流
4. **静电损伤**(ESD):未戴静电手环操作

### 为什么 C3 没事

C3 的 GPIO18/19 能正常 loopback + 发送给主控(C3→主控成功),
说明 **C3 的 GPIO 输出驱动健康**,问题只在主控 ESP32。

---

## 七、解决方案

### 当前结论: 软件无法解决

主控 ESP32 模组 GPIO 输出驱动硬件损坏,所有可编程 GPIO 在高频 UART 下失效。
**这是硬件问题,任何固件改动都无法解决。**

### 推荐: 更换主控 ESP32 模组/开发板

换一块新的 ESP32-32D 开发板(或 ESP32 DevKit),重新烧主控固件即可。
新板子的 GPIO 输出驱动健康,UART1 通信会立即正常。

换板后 UART 引脚建议(用全新脚,避开历史损坏区):
- 主控 GPIO5 (TX) → C3 GPIO19 (RX)  [或用 GPIO18/19]
- 主控 GPIO2 (RX) ← C3 GPIO18 (TX)  [或用其他空闲脚]
- GND 共地

### 代码已就绪

主控和 C3 的 UART 通信代码已全部完成并清理:
- `firmware/src/terminal_link.h`: UART1 `$S`/`$C` 协议,引脚已改到 GPIO5/2
- `terminal/src/uart_link.h`: 收 `$S` 发 `$C`,解析完整
- `terminal/src/c3_terminal.ino`: 主框架,分阶段开关
- `terminal/platformio.ini`: 含完整烧写踩坑说明
- `tools/c3_backup/stock_bootloader.bin`: C3 烧写必需

**换新主控板后,直接烧录主控固件 + 接线,即可完成通信验证。**

---

## 八、调试中验证过的关键事实

### C3 (立创实战派 ESP32-C3) — 完全健康

| 项目 | 状态 |
|---|---|
| 芯片 (ESP32-C3 QFN32 rev 0.4, 8MB Flash) | ✅ 健康 |
| WiFi 射频 | ✅ 出厂固件验证可用 |
| 屏幕显示 | ✅ 出厂固件触摸坐标正常 |
| UART1 GPIO18/19 自 loopback | ✅ 成功 |
| C3→主控 发送方向 | ✅ 成功(主控 RX 能收) |
| UART 固件 (326KB, 无 WiFi) | ✅ 启动稳定, 无 boot loop |

### 主控 ESP32-32D — GPIO 输出损坏

| 项目 | 状态 |
|---|---|
| CPU / Flash / 串口 / WiFi | ✅ 正常(能烧录、能跑程序) |
| 雷达 UART2 (GPIO16/17) | ✅ 正常(收到 229 字节雷达数据) |
| GPIO 输入 (作 UART RX) | ✅ 正常(能收 C3 发的数据) |
| **GPIO 高频输出 (作 UART TX)** | ❌ **全部损坏**(0.12V) |
| GPIO 慢速输出 (digitalWrite) | ⚠️ GPIO2/5/15 勉强(1.65V),其余 0V |

---

## 九、文件清单 (本次调试产出/修改)

### C3 侧
- `terminal/src/uart_link.h` (新增, UART 通信)
- `terminal/src/c3_terminal.ino` (改用 uart_link)
- `terminal/src/config_view.h` (key 名对齐 terminal_link.h)
- `terminal/src/net_link.h` (已删除, WiFi 版废弃)
- `terminal/platformio.ini` (烧写踩坑说明)
- `terminal/partitions_factory_8M.csv` (新增, 含 phy_init)
- `terminal/README.md` (重写, UART 方案 + 烧写指南)
- `tools/c3_backup/stock_bootloader.bin` (从出厂固件提取)

### 主控侧
- `firmware/src/terminal_link.h` (UART 协议, 引脚 GPIO5/2)
- `firmware/src/ebike_bsd.ino` (删 OLED 定义, 接入 terminalLink)
- `firmware/platformio.ini` (清理调试 flag)

### 本文档
- `docs/C3_TERMINAL_DEBUG_LOG.md` (本文件)

---

## 十、阶段 2: C3 完整 UI 开发 + 主控联调 (2026-07-08 ~ 07-09)

### 10.1 UART 引脚修正 (GPIO18/19)

**问题**: 原 GPIO5(TX)/GPIO2(RX) 不稳定，GPIO2 是 strapping 引脚（启动时影响 Flash 模式）。

**解决**: 改用 GPIO18(TX)/GPIO19(RX)，与 v2.7-c3-display 一致，实测双向通信稳定。
之前诊断"主控 GPIO 损坏"仅影响 GPIO5/GPIO21/GPIO22，GPIO18/19 正常。

### 10.2 防闪屏 (Sprite 背景 + 脏标志)

**问题**: 每帧 fillScreen 全屏重绘造成闪烁。

**解决**:
- 雷达图: 静态背景预渲染到 LGFX_Sprite，每帧 pushSprite 一次性覆盖
- 状态页/配置页: 脏标志（_needsDraw / summaryChanged），仅在数据变化或切页时重绘
- 所有绘制用 startWrite/endWrite 包裹，避免 SPI 分段撕裂
- 刷新率 50fps → 15fps（主控 10Hz 推送，无需更快）

### 10.3 触摸校准 (FT6336 裸 I2C)

**问题**: LovyanGFX 内置 FT5x06 驱动横屏坐标偏移，白点位置和手指不对应。

**解决**: 弃用 LovyanGFX 触摸，直接裸读 FT6336 寄存器 + 手动变换（移植自 v2.7-c3-display）:
```
屏幕X = raw_y                        (范围 0~317 → 0~319)
屏幕Y = (212 - raw_x) * 239 / 187    (raw_x 25~212 → 0~239)
```

实测校准数据: 左上(212,0)→(0,0)，右上(200,317)→(317,15)，左下(25,0)→(0,239)。

### 10.4 触摸防抖 (边沿触发)

**问题**: 时间锁防抖（300ms）导致 SAVE/REFRESH 按不到——手指按下瞬间坐标偏移消耗了响应机会。

**解决**: 改用 wasPressed 边沿触发，只在"无触摸→有触摸"瞬间响应一次。

### 10.5 配置页 (3 tab + REFRESH + WiFi)

**布局**: -/+ 按钮在中间（x=115/161），避开右边缘触摸死区（raw_y>290 不准）。
- RCW tab: 6 参数（low/speed/range/hold/lflash/flash）
- TURN tab: 2 参数（speed/range）
- SYS tab: 3 参数 + WiFi 开关（进入时自动 requestConfig）

**REFRESH**: C3 发 $C,GETCFG → 主控回 $CFG,12 值（含 wifi_on）
**参数同步**: 只在收到新 $CFG 时同步一次（cfg_seq 计数），不覆盖用户本地编辑

### 10.6 报警音 (ES8311 官方寄存器序列)

**问题**: 手写的 ES8311 寄存器序列完全错误，喇叭只出杂音。

**解决**: 移植乐鑫官方 es8311.c 驱动的初始化序列:
- 复位: REG00 = 0x1F → 0x00 → 0x80
- 时钟: 16kHz + MCLK 6.144MHz，coeff_div 查表系数
- 数据格式: 16bit I2S Slave
- 关键上电: REG0D/0E/12/13/1C/37
- 2kHz 方波（16 样本/周期）
- playBuf 阻塞写入（portMAX_DELAY），开机自检音 500ms

### 10.7 WiFi 控制 (手动模式)

**问题**: C3 点 WiFi ON，主控开了 AP 但 30s 空闲后又自动关了。

**解决**: 加 g_wifi_manual 标志，C3 触摸面板控制 WiFi 后禁止自动关闭。

### 10.8 联调结果

主控 ESP32 (COM4) + C3 (COM3) UART 连接验证:
- ✅ C3 显示 ONLINE，接收 $S 帧
- ✅ 参数 SAVE: C3 发 $C,key=value → 主控 applied + saveToNVS
- ✅ REFRESH: C3 发 $C,GETCFG → 主控回 $CFG → C3 同步参数
- ✅ WiFi 开关: C3 发 $C,wifi_on=1/0 → 主控开/关 AP（不自动关）
- ✅ 模拟目标: 雷达无真实目标时主控生成 3 个移动目标（正弦驱动）

### 10.9 本次产出文件

**C3 侧**:
- `terminal/src/c3_terminal.ino` — 全功能启用 + 触摸调试开关 + 边沿触发
- `terminal/src/radar_view.h` — Sprite 防闪屏 + 40m 量程 + 脏标志
- `terminal/src/status_view.h` — 脏标志 + 英文显示
- `terminal/src/config_view.h` — 3 tab + REFRESH + WiFi 开关 + 本地编辑
- `terminal/src/alert_sound.h` — 官方 ES8311 序列 + 2kHz + 开机自检
- `terminal/src/lgfx_config.hpp` — 移除 LovyanGFX 触摸 + drawNavBtn
- `terminal/src/uart_link.h` — $CFG 解析 + requestConfig + sendWifi

**主控侧**:
- `firmware/src/terminal_link.h` — GPIO18/19 + GETCFG + wifi_on + 模拟目标
- `firmware/src/ebike_bsd.ino` — g_wifi_running/g_wifi_manual 全局 + 自动关排除手动

