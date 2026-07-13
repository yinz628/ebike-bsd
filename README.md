# ebike-bsd — 电动自行车盲区监测 + 转向灯辅助系统

基于 ESP32 + 60GHz 毫米波雷达的电动自行车安全辅助系统, 实时检测后方来车,
通过 LED 指示灯 / 蜂鸣器 / 车把显示屏三级报警, 并提供转向灯自动控制.

> **版本:** V3.0 | **状态:** 项目冻结 (暂停迭代)

---

## 功能

- **后方盲区监测 (BSD)**: 雷达实时检测后方目标, 盲区有车时 LED 慢闪
- **后碰撞预警 (RCW)**: 后方快速接近时 LED 快闪 + 蜂鸣器 4Hz 报警
- **转向灯辅助**: 打转向灯时检测同侧危险, 自动长鸣警示
- **车把终端显示**: ESP32-C3 + ST7789 屏幕实时显示雷达扇形图 / 状态 / 参数配置
- **WiFi 配置台**: 手机连热点 192.168.4.1 实时调参 (角度/速度/距离阈值)
- **OTA 升级**: 手机网页一键升级主控和终端固件 (GitHub Action + jsDelivr CDN)

## 硬件

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 MCU | ESP32-32D | 车尾, 负责雷达处理 + 转向灯 + 蜂鸣器 + WiFi |
| 车把终端 | ESP32-C3 (立创实战派) | 车把, ST7789 2寸屏 + FT6336 触摸 + ES8311 音频 |
| 雷达 | MS60-3015 60GHz | 居中安装, 921600 baud UART |
| LED | IRLZ44N MOSFET 驱动 | 4 路 (左/右转向灯 + 左/右指示灯) |
| 蜂鸣器 | 12V 高分贝 | 后方碰撞时 4Hz 报警 |

## 快速开始

### 1. 克隆 + 构建

```bash
git clone https://github.com/yinz628/ebike-bsd.git
cd ebike-bsd

# 安装 PlatformIO (如未装)
pip install platformio

# 构建主控固件
cd firmware && pio run

# 构建车把终端固件
cd ../terminal && pio run
```

### 2. 烧录

**主控 ESP32** (USB 连接, PlatformIO 自动烧录):
```bash
cd firmware && pio run -t upload
```

**车把终端 C3** (首次需用 esptool 一次性重刷分区表 + bootloader + 固件):
```bash
cd terminal && pio run   # 先构建
python -m esptool --port COM3 --chip esp32c3 --baud 460800 --after no_reset write_flash \
  0x0     tools/c3_backup/stock_bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x20000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

> ⚠ C3 必须用 stock bootloader (PlatformIO 编译的会卡死), 见 `terminal/platformio.ini` 注释.
> 后续升级可走 OTA, 无需再接线. 详见 [OTA.md](OTA.md).

### 3. 使用

1. 主控上电后 WiFi AP 自动开启 (SSID: `eBike-BSD`, 密码 `12345678`)
2. 手机连接热点, 浏览器打开 `192.168.4.1` 调参
3. 30 秒无连接自动关 WiFi 省电; C3 触摸屏可重新开启

## 项目结构

```
ebike-bsd/
├── firmware/src/          主控 ESP32 固件
│   ├── ebike_bsd.ino      主程序 (setup + loop)
│   ├── ota_manager.h      OTA 升级 (上传 + C3 转发 + 回滚保护)
│   ├── wifi_web.h         WiFi AP + Web 控制台
│   ├── terminal_link.h    主控→C3 UART 链路
│   ├── config_store.h     JSON 配置 + NVS 持久化
│   ├── ms60_radar.h       MS60-3015 雷达驱动
│   ├── bsd_protocol.h     BSD 协议解析
│   ├── led_control.h      LED PWM 控制
│   └── types.h            跨文件共享类型
├── terminal/src/          车把终端 C3 固件
│   ├── c3_terminal.ino    主程序 (显示 + 触摸 + OTA)
│   ├── uart_link.h        C3←主控 UART 通信
│   ├── ota_receiver.h     OTA 接收 (分块 + CRC + 进度)
│   ├── ota_guard.h        回滚保护 (boot guard)
│   ├── base_view.h        视图抽象基类
│   ├── radar_view.h       雷达扇形图
│   ├── status_view.h      系统状态页
│   ├── config_view.h      参数配置页 (触摸调参)
│   ├── alert_sound.h      ES8311 报警音
│   └── theme.h            主题色定义
├── protocol/              主控↔C3 共享协议头
│   └── term_protocol.h    TERM_BAUD / OTA_BLOCK_BYTES / CRC16 (单一真源)
├── .github/workflows/     CI/CD
│   └── build-release.yml  tag 触发自动构建发布到 gh-pages
├── docs/                  设计文档 (见下方)
├── kicad/                 PCB 工程文件
└── tools/                 辅助脚本 (C3 bootloader 备份等)
```

## 文档

| 文档 | 内容 |
|---|---|
| [DESIGN.md](docs/DESIGN.md) | 完整设计文档 (硬件选型/电路/安装/调试/功耗/版本日志) |
| [OTA.md](OTA.md) | OTA 升级指南 (发布/使用/安全/测试结果/已知问题) |
| [使用手册.md](docs/使用手册.md) | 安装与使用手册 (面向用户) |
| [实车测试清单.md](docs/实车测试清单.md) | 实车测试检查表 |
| [MAINTAINABILITY_REVIEW.md](MAINTAINABILITY_REVIEW.md) | 长期可维护性审查报告 |
| [PCB_DESIGN.md](docs/PCB_DESIGN.md) | PCB 设计说明 |
| [EASYEDA_BOM.md](docs/EASYEDA_BOM.md) | 物料清单 |
| [terminal/README.md](terminal/README.md) | C3 终端接线/烧录/协议说明 |

## 发布固件 (OTA)

```bash
# 改版本号 (firmware/src/ebike_bsd.ino 的 FW_VERSION)
# 提交后打 tag:
git tag v2.8
git push origin v2.8
# GitHub Action 自动构建 → gh-pages 分支 → jsDelivr CDN
# 手机网页 192.168.4.1 即可一键升级
```

## 技术栈

- **MCU**: ESP32 (Xtensa LX6) + ESP32-C3 (RISC-V)
- **框架**: Arduino-ESP32 + PlatformIO
- **显示**: LovyanGFX (ST7789 320x240)
- **触摸**: FT6336 (裸 I2C)
- **音频**: ES8311 codec (I2S)
- **Web**: ESPAsyncWebServer + AsyncTCP
- **配置**: ArduinoJson + NVS (Preferences)
- **CI/CD**: GitHub Actions + jsDelivr CDN

## 安全免责

> 本系统为 DIY 辅助设备, 不能替代骑车人自己观察判断. 骑行时仍需自行观察路况.

## 许可

个人 DIY 项目, 无商业许可.
