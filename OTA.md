# OTA 升级指南

本系统支持通过手机网页对**主控 ESP32**和**车把终端 C3**进行无线固件升级。
ESP32 只作为 WiFi 热点(不联网),固件通过 jsDelivr CDN 分发,手机用浏览器拉取后推送。

> **实车测试状态**: 主控 OTA 已实车验证通过(升级+回滚); C3 OTA 协议已验证(接收正常), 完整传输待解决主控长时转发重启问题(见末尾"已知问题")。

## 一、首次准备(一次性)

### 1. 主控 ESP32
主控的分区表 `firmware/platformio.ini` 用 PlatformIO 默认 `default.csv`,**已自带 `ota_0`/`ota_1`/`otadata` 双槽**,无需任何分区改动。

只需首次用烧录器烧入带 OTA 功能的固件:
```bash
cd firmware
pio run -t upload
```
之后所有升级都走无线 OTA。

### 2. 车把终端 C3(⚠ 一次性破坏性重刷)
C3 原分区表是单 `factory`(无 OTA 槽),需切到 `partitions_ota_8M.csv`。这是一次性操作,之后所有升级都走 OTA。

按 `terminal/platformio.ini` 顶部注释里的命令,用 esptool 重刷三个文件:
```bash
cd terminal
pio run   # 先编译产出 .bin
python -m esptool --port COM3 --chip esp32c3 --baud 460800 \
  --after no_reset write_flash \
  0x0     tools/c3_backup/stock_bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x20000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```
> ⚠ 必须用 stock bootloader(PlatformIO 编译的会卡死),且 app 偏移是 `0x20000`(不是原来的 `0x10000`)。
> 万一变砖,按 **BOOT+RESET** 进下载模式即可恢复。

## 二、发布固件(开发者)

打 tag 即触发 GitHub Action 自动构建并发布到 `gh-pages` 分支:
```bash
git tag v2.8
git push origin v2.8
```
或去 GitHub Actions 页面手动运行 workflow(可填版本号和说明)。

Action 会:
1. 用 PlatformIO 构建 firmware + terminal
2. 计算 SHA256
3. 生成 `manifest.json`(版本/SHA/大小/说明)
4. 推到 `gh-pages` 分支(jsDelivr 自动缓存)

发布后 ~10 分钟,jsDelivr CDN 会刷新,手机端能查到新版本。

## 三、升级(用户)

1. 手机连接 WiFi 热点 `eBike-BSD`(密码 `12345678`)
2. 浏览器打开 `192.168.4.1`
3. 滚到"⬆️ 固件升级"卡片

### 一键升级(推荐,iOS / 现代安卓)
- 网页自动检查云端版本(走手机蜂窝/家里 WiFi,不经 ESP32)
- 显示新版本号和说明
- 点"🚀 一键升级主控"或"📱 一键升级车把终端"
- 等进度条跑完 → 主控自动重启 / C3 屏幕显示进度后重启

> iOS 连无网热点时会自动回落蜂窝数据访问 jsDelivr,所以一键可用。
> 部分安卓若不回落,会显示"无法连接云端",改用下面的手动上传。

### 手动上传(离线/降级兜底)
1. 展开"手动上传"
2. 先在有网环境用电脑/手机下载 `.bin`(从 GitHub `gh-pages` 分支或 Releases)
3. 切回 eBike-BSD 热点
4. 选文件 → 上传

## 四、安全机制

- **A/B 双槽 + 自动回滚**:新固件写入对面槽,首次启动后未确认前,若 panic/看门狗重启,bootloader 自动回滚到上一好槽。**永不真砖**。
- **SHA-256 校验**:`Update.end(true)` 写入时校验镜像完整性,bootloader 启动时二次校验。
- **CRC16 + ACK/NACK**:C3 经 UART 转发时,每块校验并要求确认,出错自动重传(最多 3 次)。
- **看门狗安全**:主控 5s TWDT,OTA 写 Flash 前后按 `setup()` 既有的 `esp_task_wdt_deinit()/reinit` 模式包裹。

## 五、架构与协议细节

```
┌───────┐  WiFi AP(无互联网)    ┌────────主控 ESP32────────┐  UART1  ┌──C3 终端──┐
│ 手机  │ ─上传 firmware.bin───→│ /api/ota_main            │         │           │
│ 浏览器│                       │  → Update() 写 ota_1     │         │           │
│       │ ─上传 c3.bin─────────→│  → ESP.restart()         │         │           │
│       │  (jsDelivr CDN 拉取)  │ /api/ota_c3              │         │           │
│       │                       │  → SPIFFS 暂存           │──$OTAC─→│ Update()  │
└───────┘                       │  → UART1 转发状态机      │←─$C,OTAR│ 写ota_1   │
                                └──────────────────────────┘         └───────────┘
```

**主控↔C3 UART 协议**(ASCII 行,见 `firmware/src/ota_manager.h` 和 `terminal/src/uart_link.h`):
- `$OTAB,<size>,<version>` 开始
- `$OTAC,<seq>,<hex128B>,<crc16>` 分块(128 字节裸数据编码成 hex)
- `$OTAE` 结束
- `$C,OTAR,ready` / `$C,OTAR,<seq>` ACK
- `$C,OTAN,<seq>` NACK(重传)
- `$C,OTAOK` / `$C,OTAFAIL,<reason>` 结束

分块 128 字节,C3 固件 ~385KB ≈ 3070 块,115200 波特率约 1.5~2 分钟完成。

## 六、文件清单

| 文件 | 作用 |
|---|---|
| `firmware/src/ota_manager.h` | 主控 OTA 核心(上传端点 + SPIFFS 暂存 + C3 转发状态机 + 回滚保护) |
| `firmware/src/wifi_web.h` | Web UI 升级卡片 + JS 一键/手动上传 |
| `firmware/src/terminal_link.h` | C3 OTA 回复(OTAR/OTAN/OTAOK/OTAFAIL)分发 |
| `terminal/src/uart_link.h` | C3 OTA 接收(Update.write + ACK/NACK + 回滚保护) |
| `terminal/src/c3_terminal.ino` | C3 升级中屏幕提示 + 回滚确认 |
| `terminal/partitions_ota_8M.csv` | C3 OTA 双槽分区表 |
| `.github/workflows/build-release.yml` | 自动构建 + 发布 manifest 到 gh-pages |

## 七、实车测试结果 (2026-07-12)

### 主控 ESP32 OTA — ✅ 全部通过

| 测试 | 结果 | 说明 |
|---|---|---|
| 版本号 + 启动 | ✅ | V2.8 串口横幅, SPIFFS/WiFi/Web 全部正常 |
| OTA 上传升级 | ✅ | ota_0→ota_1, 996KB / 7.4s, SHA-256 校验通过 |
| 自动回滚 | ✅ | 坏固件(setup死循环)→3 次重启→boot guard 触发→回 ota_0 |

### 分发链路 — ✅ 通过

| 测试 | 结果 | 说明 |
|---|---|---|
| GitHub Action 构建 | ✅ | tag v2.8 触发, 主控+C3 固件 + manifest.json 发布 |
| jsDelivr CDN | ✅ | manifest.json HTTP 200, 内容完整 |
| 手机网页云端检查 | ✅ | 显示"云端最新 V2.8" |

### C3 终端 OTA — ⚠️ 协议通过, 完整传输待解决

| 测试 | 结果 | 说明 |
|---|---|---|
| 分区表重刷 | ✅ | bootloader 识别 ota_0/ota_1/otadata 双槽 |
| OTA 协议接收 | ✅ | 修复解析 bug 后, OTAR 持续递增 940+ 块无 NACK |
| 屏幕进度显示 | ✅ | 进度条正常更新 |
| 完整文件传输 | ❌ | 主控在转发 ~940/3316 块时重启, 根因待定位 |

## 八、实车测试中发现并修复的真实问题

### 问题 1: Arduino-ESP32 core 自动确认新 OTA 固件 (主控)

**现象**: 新 OTA 固件 panic/WDT 重启后, bootloader 不自动回滚, 卡在 boot loop.

**根因**: Arduino-ESP32 core 的 `initArduino()` (在 `app_main`, 早于 `setup()`) 默认调用
`esp_ota_mark_app_valid_cancel_rollback()`, 把新槽秒标记 `ESP_OTA_IMG_VALID`,
绕过 `ESP_OTA_IMG_PENDING_VERIFY` → bootloader 回滚机制完全失效.

**修复** (`firmware/src/ota_manager.h` + `terminal/src/uart_link.h`):
- 覆盖 weak 函数 `verifyRollbackLater()` 返回 true, 阻止 core 自动确认
- 新增 `otaBootGuardBegin()` / `c3OtaBootGuardBegin()`: NVS 计数每次启动尝试,
  连续 3 次 setup 未完成 → 主动 `esp_ota_mark_app_invalid_rollback_and_reboot()`

### 问题 2: C3 $OTAC 帧解析 bug

**现象**: C3 收到 $OTAC 帧后全部 NACK, 卡在 0/3317.

**根因**: `handleOtaChunk` 误以为帧有 3 个逗号(4 字段), 实际 `$OTAC,<seq>,<hex>,<crc>`
只有 2 个逗号(3 字段), 第 3 个逗号 `p3` 永远找不到 → 全部 NACK.

**修复** (`terminal/src/uart_link.h`): 只找 p1/p2 两个逗号, crc 取 p2 之后到行尾.

### 问题 3: GitHub Action deploy 步骤 403

**现象**: workflow 所有构建步骤成功, 仅 `Deploy to gh-pages` 失败.

**根因**: GitHub Actions 2023+ 默认 `GITHUB_TOKEN` 只读, 推 gh-pages 分支需写权限.

**修复** (`.github/workflows/build-release.yml`): 加 `permissions: contents: write`.

## 九、已知问题 (待解决)

### C3 OTA 完整传输时主控重启

**现象**: 主控转发 C3 固件到约 940/3316 块(~175 秒)时主控重启, C3 没收到完整 `$OTAE`,
OTA 无法完成. 主控空闲时稳定(30 秒监测 0 次重启).

**已排除的原因**:
- ❌ WiFi 30s 自动关 (转发期间 `sta=1` 有设备连接, 不会关)
- ❌ 单次 loop 超时 WDT (每块 ~186ms, 远低于 5s)

**可能原因** (未验证):
- 长时间转发导致堆碎片/内存累积
- SPIFFS File 长期打开
- WiFi 协议栈事件未及时处理

**下一步排查方向**:
- 加周期诊断日志(进度 + `ESP.getFreeHeap()`)抓崩溃前最后状态
- 提高波特率 115200→460800 缩短传输时间(3316 块从 ~10 分钟降到 ~2.5 分钟)
- 用手机上传(稳定 WiFi) + 串口监控, 排除测试环境网络抖动干扰

