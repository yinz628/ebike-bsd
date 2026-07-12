# OTA 升级指南

本系统支持通过手机网页对**主控 ESP32**和**车把终端 C3**进行无线固件升级。
ESP32 只作为 WiFi 热点(不联网),固件通过 jsDelivr CDN 分发,手机用浏览器拉取后推送。

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
| `firmware/src/ota_manager.h` | 主控 OTA 核心(上传端点 + SPIFFS 暂存 + C3 转发状态机) |
| `firmware/src/wifi_web.h` | Web UI 升级卡片 + JS 一键/手动上传 |
| `firmware/src/terminal_link.h` | C3 OTA 回复(OTAR/OTAN/OTAOK/OTAFAIL)分发 |
| `terminal/src/uart_link.h` | C3 OTA 接收(Update.write + ACK/NACK) |
| `terminal/src/c3_terminal.ino` | C3 升级中屏幕提示 + 回滚确认 |
| `terminal/partitions_ota_8M.csv` | C3 OTA 双槽分区表 |
| `.github/workflows/build-release.yml` | 自动构建 + 发布 manifest 到 gh-pages |
