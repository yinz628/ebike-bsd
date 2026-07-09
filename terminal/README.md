# ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)

把立创实战派 ESP32-C3 开发板作为 ebike-bsd 系统的可视化终端，显示雷达目标可视化、系统状态，并支持触摸配置参数。

**状态：✅ 全功能已验证通过**（主控 ESP32 ↔ C3 UART 双向通信正常）

## 功能一览

| 功能 | 说明 |
|---|---|
| 雷达扇形图 | 40m 量程，目标红点 + 速度标注，4 条距离弧线，±40° FOV |
| 状态页 | 主控连接状态 / 运行时间 / 雷达字节 / 转向 / 蜂鸣 / 目标列表 |
| 配置页 | 3 个 tab（RCW 6参 / TURN 2参 / SYS 3参 + WiFi 开关），触摸调参 + SAVE |
| REFRESH | 查询主控当前配置，同步到 C3 |
| WiFi 控制 | C3 触摸面板开关主控 WiFi AP（手动控制不自动关） |
| 报警音 | ES8311 + NS4150B 功放，2kHz 方波，4 种蜂鸣模式 |
| 触摸 | FT6336 裸 I2C 读取 + 横屏校准，边沿触发防抖 |

## 通信方案：UART 有线

C3 作为从机，通过 UART1 接收主控推送的雷达状态帧（10Hz），触摸配置通过 UART 回传主控：

```
车尾主控 ESP32-32D (GPIO18/19, UART1)
  │ $S 状态帧 10Hz 推送 (主控→C3)
  │ $C 配置命令 (C3→主控, 触摸调参/WiFi 开关)
  ▼ UART 115200bps
车把终端 实战派 ESP32-C3 (GPIO18/19, UART1 多功能口)
  ST7789 屏: 雷达扇形图 + 状态 + 配置
  FT6336 触摸: 切页/调参
  ES8311 扬声器: 报警音同步
```

### 接线

| 主控 ESP32-32D | C3 实战派 (多功能口) | 方向 |
|---|---|---|
| GPIO18 (UART1 TX) | GPIO19 (UART1 RX) | 主控 → C3 |
| GPIO19 (UART1 RX) | GPIO18 (UART1 TX) | C3 → 主控 |
| GND | GND | 共地（必须） |

> **为什么不用 WiFi？** 实测主控 ESP32 长时间 WiFi 射频会发热；C3 的 WiFi 固件（840KB）在 bootloader 加载大段时不稳定（boot loop）。改 UART 后：主控不发热 + C3 固件缩到 371KB（启动稳定）+ 延迟 < 100ms。
>
> 主控的 WiFi AP 仍保留（开机 30s 自动关或 C3 手动控制），与 UART 终端互不干扰。

## 协议

### 主控 → C3：`$S` 状态帧（10Hz）

```
$S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid[,t_range,t_angle,t_velo,t_id]...\n
```

### 主控 → C3：`$CFG` 配置回传（C3 请求时）

```
$CFG,rcw_low,rcw_speed,rcw_range,rcw_hold,rcw_lflash,rcw_flash,turn_speed,turn_range,beep_cool,det_range,sensitivity,wifi_on\n
```

### C3 → 主控：`$C` 配置命令

```
$C,key=value\n     # 设置参数（主控收到后自动 saveToNVS + setBSDMode）
$C,SAVE\n          # 保存配置（已废弃，SAVE 按钮改为逐个发 key=value）
$C,GETCFG\n        # 查询主控当前配置（主控回 $CFG 帧）
$C,wifi_on=1/0\n   # WiFi AP 开关
```

支持的 key（与主控 `terminal_link.h` 的 `applyCommand()` 一致）：

| tab | key | 含义 |
|---|---|---|
| RCW | rcw_low, rcw_speed, rcw_range, rcw_hold, rcw_lflash, rcw_flash | 后方碰撞预警参数 |
| TURN | turn_speed, turn_range | 转向辅助参数 |
| SYS | sensitivity, det_range, beep_cool | 雷达灵敏度/探测距离/蜂鸣冷却 |
| SYS | wifi_on | WiFi AP 开关 |

## 触摸校准（FT6336）

不用 LovyanGFX 内置 FT5x06 驱动（横屏旋转坐标有偏移），改用**裸 I2C 读取 + 手动变换**（移植自 v2.7-c3-display）：

```cpp
// FT6311 寄存器 0x02 起 6 字节: [点数, xH, xL, yH, yL, ...]
int raw_x = ((buf[1] & 0x0F) << 8) | buf[2];  // 竖屏方向, 25~212
int raw_y = ((buf[3] & 0x0F) << 8) | buf[4];  // 竖屏方向, 0~317
// 横屏变换 (实测校准):
*x = raw_y;                          // → 0~317 (屏幕宽 320)
*y = (212 - raw_x) * 239 / 187;     // → 0~239 (屏幕高 240)
```

触摸防抖用**边沿触发**（`wasPressed` 标志）：只在"无触摸→有触摸"瞬间响应一次，避免按住期间重复触发。

## 硬件关键配置

### C3 platformio.ini 必需配置

```ini
board_build.f_flash = 80000000L     ; 与 stock bootloader 一致
board_build.flash_mode = dio
board_build.partitions = partitions_factory_8M.csv   ; 含 phy_init 分区
build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=0     ; Serial 走 CH343 (UART0)
    -D ARDUINO_USB_HID_ON_BOOT=0     ; 释放 GPIO18/19 给 UART1
```

### 引脚一览（对照官方 IDF 例程核对）

| 项目 | 引脚 | 来源 |
|---|---|---|
| 屏幕 SPI | MOSI=5/SCK=3/CS=4/DC=6 | 07-spi_lcd |
| 背光 | GPIO2，**低电平点亮** | 官方 BK_LIGHT_ON_LEVEL=0 |
| 触摸 FT6336 | I2C 0x38，SDA=0/SCL=1 | 08-spi_lcd_touch |
| 音频 I2S | BCK=8/WS=12/DOUT=11/DIN=7/MCLK=10 | 06-i2s_es8311 |
| ES8311 codec | I2C 0x18，共用 SDA=0/SCL=1 | 同触摸总线 |
| 功放 NS4150B | GPIO13，**高电平使能** | 06-i2s_es8311 |
| UART1 多功能口 | GPIO18(TX)/GPIO19(RX) | wiki |

### ES8311 寄存器初始化

移植自乐鑫官方 `es8311.c` 驱动（`managed_components/espressif__es8311`）。关键配置：
- 16kHz / 16bit / I2S Slave / MCLK 6.144MHz（从 MCLK pin）
- 复位序列：REG00 = 0x1F → 0x00 → 0x80
- 时钟系数：coeff_div[6144000, 16000] 查表值
- 2kHz 方波样本（16 样本/周期，半周期 8 样本）

## ⚠ 烧写指南

### 首次烧写 / 换分区表（烧三个文件）

ESP32-C3 的 bootloader 烧到 **0x0**（不是 0x1000）。必须用 stock bootloader：

```bash
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x0     tools/c3_backup/stock_bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

### 日常更新固件（只换 app）

```bash
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

> `--after no_reset` 让 C3 烧完后**停留在下载模式**，下次可直连。
> 若 C3 已启动运行，需按 **BOOT + RESET** 手动进下载模式（Windows CH343 自动复位不可靠）。

## 文件结构

```
terminal/
├── platformio.ini             C3 工程配置（含烧写说明）
├── partitions_factory_8M.csv   分区表（含 phy_init）
├── src/
│   ├── c3_terminal.ino        主框架（UART + 触摸切页 + 报警音）
│   ├── uart_link.h            UART 通信（$S 帧 / $C 命令 / $CFG 解析）
│   ├── lgfx_config.hpp        LovyanGFX ST7789 配置 + drawNavBtn
│   ├── radar_view.h           雷达扇形可视化（Sprite 防闪屏）
│   ├── status_view.h          系统状态页
│   ├── config_view.h          参数配置页（3 tab + REFRESH + WiFi 开关）
│   └── alert_sound.h          ES8311 报警音（官方寄存器序列）
└── README.md

tools/c3_backup/
└── stock_bootloader.bin       从出厂固件提取的 bootloader
```

主控侧改动：
- `firmware/src/terminal_link.h` — UART1 链路（$S/$C/$CFG 协议 + WiFi 控制 + 模拟目标）
- `firmware/src/ebike_bsd.ino` — `g_wifi_running` / `g_wifi_manual` 全局变量 + 接入 terminalLink
