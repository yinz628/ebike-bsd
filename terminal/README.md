# ebike-bsd 车把显示终端 (立创·实战派 ESP32-C3)

把立创实战派 ESP32-C3 开发板作为 ebike-bsd 系统的可视化终端,显示雷达目标可视化、系统状态,并支持触摸配置参数。

## 通信方案:WiFi (当前实现)

C3 作为 WiFi STA 连接车尾主控的 AP(`eBike-BSD`),通过 HTTP 轮询主控现有 Web API:

```
车尾主控 ESP32-32D (现有, 无需改动)
  WiFi AP "eBike-BSD" 192.168.4.1
    GET  /api/status   ← C3 每 200ms 轮询取目标/状态
    GET  /api/config   ← C3 读取配置
    POST /api/config   ← C3 触摸提交参数 (主控自动 saveToNVS + setBSDMode)
    POST /api/reset    ← C3 出厂重置
        │ WiFi (HTTP)
        ▼
车把终端 实战派 ESP32-C3
  ST7789 屏: 雷达扇形图 + 状态 + 配置
  FT6336 触摸: 切页/调参
  ES8311 扬声器: 报警音同步 (按 buzzer 字段)
```

**延迟 ~200ms**,对目标可视化足够;主控车尾蜂鸣器是实时主警报源,C3 扬声器为补充提醒。

> 历史上尝试过 UART 有线方案(代码保留在 `firmware/terminal_link.h`),但因主控 ESP32-32D 上 UART1 引脚重映射在完整项目代码下不生效(纯测试程序可以,带 WiFi/雷达的完整代码不行),最终采用 WiFi 方案。

## 分阶段开关

`c3_terminal.ino` 顶部用条件编译控制各模块,便于逐阶段验证:

```cpp
// #define ENABLE_DISPLAY      // 屏幕初始化 (LovyanGFX)
// #define ENABLE_RADAR_VIEW   // P2: 雷达扇形图
// #define ENABLE_STATUS_VIEW  // P3: 状态页
// #define ENABLE_CONFIG_VIEW  // P3: 配置页 + 触摸
// #define ENABLE_ALERT_SOUND  // P4: ES8311 报警音
```

P1 阶段全关,只验证 WiFi 连接 + HTTP 取数据 + 串口打印心跳。

## 硬件关键配置 (调试中确认)

### C3 platformio.ini 必需的 build_flags

```ini
board_build.f_flash = 40000000L     ; ⚠ 必需! 实战派 Flash 不支持 80MHz, 否则 boot loop
board_build.flash_mode = dio
build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=0     ; Serial 走 CH343 (UART0/GPIO21-20), 不走原生 USB
    -D ARDUINO_USB_HID_ON_BOOT=0     ; 释放 GPIO18/19 的 USB-HID 占用 (留给 UART1, 若用有线方案)
```

### 已确认的硬件事实

| 项目 | 值 |
|---|---|
| 芯片 | ESP32-C3 (QFN32 rev 0.4), 8MB Flash |
| USB 转串口 | CH343 (接 GPIO21 TX0 / GPIO20 RX0, 即 UART0) |
| 屏幕 | ST7789, 2.0寸 320×240, SPI (MOSI=5/SCK=3/CS=4/DC=6/BL=2) |
| 触摸 | FT6336, I2C 0x38 (SDA=8/SCL=9) |
| 音频 | ES8311 (I2S+I2C) |
| 多功能口 | GPIO18/19 (默认 USB-HID, 需上述 flag 释放后可做 UART) |

## 当前进度

| 阶段 | 状态 |
|:-:|:-:|
| C3 boot loop 修复 (Flash 40MHz) | ✅ 已解决 |
| C3 UART1 GPIO18/19 可用 (USB HID flag) | ✅ 已解决 (loopback 验证) |
| 主控 terminal_link UART 协议 | ✅ 代码完成, 但主控 UART1 引脚重映射在完整代码下不生效 |
| 转 WiFi 方案 | ✅ 代码完成 (`net_link.h`) |
| C3 WiFi 连接主控 AP 验证 | ⏳ 待恢复 C3 后验证 |
| 屏幕显示 / 触摸 / 报警音 | ⏳ 代码完成, 待联调 |

## C3 恢复方法 (若 Serial 静默)

调试过程中若 C3 串口完全无输出(bootloader 可能损坏),恢复步骤:

1. 按住 C3 的 **BOOT** → 按一下 **RESET** → 松开 BOOT,进入下载模式
2. 用 esptool 完整烧录三个文件:
   ```bash
   python -m esptool --port COM3 --chip esp32c3 write_flash \
     0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
   ```
3. 烧完应能看到 `=== ebike-bsd C3 终端 ===` 启动横幅

## 文件结构

```
terminal/
├── platformio.ini             C3 工程配置 (含 40MHz/HID 等 flags)
├── src/
│   ├── c3_terminal.ino        主框架 (WiFi + 心跳 + 触摸切页)
│   ├── net_link.h             WiFi HTTP 客户端 (GET /api/status, POST /api/config)
│   ├── lgfx_config.hpp        LovyanGFX ST7789 + FT6336 配置
│   ├── radar_view.h           雷达扇形可视化 (移植自主控 web Canvas)
│   ├── status_view.h          系统状态页
│   ├── config_view.h          参数配置页 + 触摸调参
│   └── alert_sound.h          ES8311 报警音同步
└── README.md                  本文件
```

主控侧改动:`firmware/src/terminal_link.h`(UART 方案,保留备用)+ `ebike_bsd.ino` 的 init/update 调用。WiFi 方案下主控固件**无需任何改动**(直接复用现有 Web API)。
