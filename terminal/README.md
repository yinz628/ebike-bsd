# ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)

把立创实战派 ESP32-C3 开发板作为 ebike-bsd 系统的可视化终端,显示雷达目标可视化、系统状态,并支持触摸配置参数。

## 通信方案:UART 有线 (当前实现)

C3 作为从机,通过 UART1 接收主控推送的雷达状态帧,延迟 <100ms:

```
车尾主控 ESP32-32D (GPIO21/22, UART1)
  │ $S 状态帧 10Hz 推送 (主控→C3)
  │ $C 配置命令 (C3→主控, 触摸调参时)
  ▼ UART 115200bps
车把终端 实战派 ESP32-C3 (GPIO18/19, UART1 多功能口)
  ST7789 屏: 雷达扇形图 + 状态 + 配置
  FT6336 触摸: 切页/调参
  ES8311 扬声器: 报警音同步 (按 buzzer 字段)
```

### 接线

| 主控 ESP32-32D | C3 实战派 (多功能口) | 方向 |
|---|---|---|
| GPIO21 (UART1 TX) | GPIO18 (UART1 RX) | 主控 → C3 |
| GPIO22 (UART1 RX) | GPIO19 (UART1 TX) | C3 → 主控 |
| GND | GND | 共地 (必须) |

> **为什么不用 WiFi?** 实测主控 ESP32 长时间 WiFi 射频会发热;且 C3 的 WiFi 固件(840KB)在 bootloader 加载大段时不稳定(boot loop)。改 UART 后:主控不发热(WiFi 30s 超时关)+ C3 固件缩到 326KB(启动稳定)+ 延迟从 200ms 降到 <100ms。一举三得。
>
> 主控的 WiFi AP 仍保留(开机 30s 供手机 Web 配置),与 UART 终端互不干扰。

## 分阶段开关

`c3_terminal.ino` 顶部用条件编译控制各模块,便于逐阶段验证:

```cpp
// #define ENABLE_DISPLAY      // 屏幕初始化 (LovyanGFX)
// #define ENABLE_RADAR_VIEW   // P2: 雷达扇形图
// #define ENABLE_STATUS_VIEW  // P3: 状态页
// #define ENABLE_CONFIG_VIEW  // P3: 配置页 + 触摸
// #define ENABLE_ALERT_SOUND  // P4: ES8311 报警音
```

P1 阶段全关,只验证 UART 连接 + 收 $S 帧 + 串口心跳。

## 协议

### 主控 → C3: `$S` 状态帧 (10Hz)

```
$S,obj_num,bz,rcw_l,rcw_r,ind_l,ind_r,turn,rx_bytes,valid[,t_range,t_angle,t_velo,t_id]...\n
```

| 字段 | 含义 |
|---|---|
| obj_num | 雷达检测到的目标数 |
| bz | 蜂鸣器模式 (0=关) |
| rcw_l / rcw_r | 左/右后向碰撞预警 (0/1) |
| ind_l / ind_r | 左/右指示灯模式 (0=灭 1=BSD慢闪 2=RCW快闪 3=转向常亮) |
| turn | 转向状态 (0=OFF 1=LEFT 2=RIGHT) |
| rx_bytes | 雷达累计接收字节数 |
| valid | 雷达数据是否有效 |
| t_range/t_angle/t_velo/t_id | 每个目标的距离/角度/速度/ID (4字段一组, 最多 N 组) |

### C3 → 主控: `$C` 配置命令

```
$C,key=value\n     # 设置参数 (主控收到后自动 saveToNVS + setBSDMode)
$C,SAVE\n          # 保存配置
$C,RESET\n         # 出厂重置
```

支持的 key(与主控 `terminal_link.h` 的 `applyCommand()` 一致):

| key | 含义 |
|---|---|
| rcw_speed | RCW 高警告速度 |
| rcw_low | RCW 低警告速度 |
| rcw_range | RCW 距离上限 |
| sensitivity | 雷达灵敏度 |
| beep_cool | 蜂鸣冷却时间 |
| ... | (完整列表见 terminal_link.h) |

## 硬件关键配置

### C3 platformio.ini 必需配置

```ini
board_build.f_flash = 80000000L     ; 与 stock bootloader 一致
board_build.flash_mode = dio
board_build.partitions = partitions_factory_8M.csv   ; 含 phy_init 分区
build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=0     ; Serial 走 CH343 (UART0/GPIO21-20)
    -D ARDUINO_USB_HID_ON_BOOT=0     ; 释放 GPIO18/19 给 UART1
```

### 已确认的硬件事实 (对照官方 IDF 例程核对)

| 项目 | 值 | 引脚来源 |
|---|---|---|
| 芯片 | ESP32-C3 (QFN32 rev 0.4), 8MB Flash (厂商 c2, Device 2017) | esptool |
| USB 转串口 | CH343 (接 GPIO21 TX0 / GPIO20 RX0, 即 UART0) | 实测 |
| **屏幕** | ST7789, 2.0寸 320×240, SPI: MOSI=5/SCK=3/CS=4/DC=6/**BL=2** | 07-spi_lcd 例程 |
| **背光** | GPIO2, **低电平点亮** (ON_LEVEL=0), 不用 Light_PWM | 官方 BK_LIGHT_ON_LEVEL |
| **触摸** | FT6336, I2C 0x38, **SDA=GPIO0/SCL=GPIO1** | 08-spi_lcd_touch/myi2c.h |
| **音频 I2S** | BCK=8/WS=12/DOUT=11/DIN=7/MCLK=10 | 06-i2s_es8311 (C3分支) |
| **音频 I2C** | ES8311 地址 0x18, 共用 SDA=0/SCL=1 | 同触摸总线 |
| **功放** | NS4150B, GPIO13 **高电平使能** (不拉高喇叭不响) | 06-i2s_es8311 例程 |
| 多功能口 | GPIO18/19 (默认 USB-Serial-JTAG, 作 UART1 需释放) | wiki |

> ⚠ 之前 I2C 误配 8/9(那是 ESP32-H2 的分支),正确是 **GPIO0/GPIO1**(板载传感器+触摸+ES8311 共用)。
> ⚠ 背光极性:实战派 C3 的 GPIO2 背光是**低电平点亮**,LovyanGFX 的 Light_PWM 默认高电平会反向,所以改用 `digitalWrite(2, LOW)` 手动控制。

## ⚠ 烧写指南 (重要, 踩坑总结)

### 首次烧写 / 换分区表 (烧三个文件)

ESP32-C3 的 bootloader 烧到 **0x0**(不是 ESP32 经典版的 0x1000)。且**必须用 stock bootloader**(PlatformIO 编译的 bootloader 会卡死):

```bash
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x0     tools/c3_backup/stock_bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

`stock_bootloader.bin` 从出厂固件提取(见下文"stock bootloader 提取")。

### 日常更新固件 (只换 app)

分区表和 bootloader 不变时,只烧 firmware:

```bash
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

### 烧写后 C3 不启动 / USB 消失

烧写命令带 `--after no_reset`,烧完后 C3 还在下载模式,需要手动复位:
- **按一下 RESET 键**(不按 BOOT)→ C3 进 SPI boot 运行新固件
- 若 USB 完全消失:**按住 BOOT → 按一下 RESET → 松开 BOOT** 进下载模式重烧

### stock bootloader 提取方法

从立创出厂固件 `esp32-c3/出厂测试固件/esp32_c3_all.bin` 提取前 0x5080 字节(含 image header + 3 segments + checksum + SHA256):

```bash
python -c "
import struct
d = open('esp32-c3/出厂测试固件/esp32_c3_all.bin','rb').read()
n = d[1]; off = 24
for i in range(n):
    l = struct.unpack('<I', d[off+4:off+8])[0]; off += 8 + l
pad = (16 - off % 16) % 16; off += pad + 1
pad2 = (16 - off % 16) % 16; off += pad2 + 32
open('tools/c3_backup/stock_bootloader.bin','wb').write(d[:off])
print(f'stock bootloader: {off} bytes')
"
```

提取的 bootloader 已保存在 `tools/c3_backup/stock_bootloader.bin`。

## 当前进度

| 阶段 | 状态 |
|:-:|:-:|
| C3 boot loop 修复 (UART 方案, 固件缩到 326KB) | ✅ 已解决 |
| stock bootloader + factory 分区表 (含 phy_init) | ✅ 已解决 |
| UART 协议 (主控 terminal_link.h + C3 uart_link.h) | ✅ 代码完成 |
| 主控→C3 通信 ($S 帧) | ⚠️ 代码完成, 主控 GPIO 损坏待换板验证 |
| **引脚配置修正** (I2C 0/1, I2S 8/12/11/7/10, 功放13, 背光极性) | ✅ 已完成 |
| **屏幕点亮验证 (P1)** | ✅ **已验证 (测试画面正常显示)** |
| 雷达图显示 (P2) | ⏳ 待验证 |
| 触摸 + 多页切换 (P3) | ⏳ 待验证 |
| 报警音同步 (P4) | ⏳ 待验证 |

> 注: 主控 ESP32-32D 的 GPIO 输出驱动硬件损坏(高频 UART 下全部崩溃到 0.12V),
> 需更换主控板后才能验证主控→C3 方向。当前用模拟数据验证 C3 显示/触摸/音频。
> 详见 `docs/C3_TERMINAL_DEBUG_LOG.md`。

## 文件结构

```
terminal/
├── platformio.ini             C3 工程配置 (含烧写踩坑说明)
├── partitions_factory_8M.csv   分区表 (含 phy_init, 与出厂固件对齐)
├── src/
│   ├── c3_terminal.ino        主框架 (UART + 心跳 + 触摸切页)
│   ├── uart_link.h            UART 通信 (收 $S 帧, 发 $C 命令)
│   ├── lgfx_config.hpp        LovyanGFX ST7789 + FT6336 配置
│   ├── radar_view.h           雷达扇形可视化 (移植自主控 web Canvas)
│   ├── status_view.h          系统状态页
│   ├── config_view.h          参数配置页 + 触摸调参
│   └── alert_sound.h          ES8311 报警音同步
└── README.md                  本文件

tools/c3_backup/
├── stock_bootloader.bin       从出厂固件提取的 bootloader (烧写必需)
└── ...                        调试备份
```

主控侧改动:
- `firmware/src/terminal_link.h` — UART1 链路 ($S/$C 协议)
- `firmware/src/ebike_bsd.ino` — 删除 OLED 定义释放 GPIO21/22,接入 terminalLinkInit/Update
