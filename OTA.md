# OTA 升级指南

本系统支持通过手机网页对**主控 ESP32**和**车把终端 C3**进行无线固件升级。
ESP32 只作为 WiFi 热点(不联网),固件通过 jsDelivr CDN 分发,手机用浏览器拉取后推送。

> **实车测试状态**: 主控 OTA 全通过(升级+回滚+WiFi共存); C3 OTA 全通过(协议+完整传输+超时保护)。8 个实车问题全部修复, 代码经多轮审查与重构。

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
| `firmware/src/ota_manager.h` | 主控 OTA 核心(上传端点 + SPIFFS 暂存 + C3 转发状态机 + 回滚保护 + `otaIsBusy()`/`otaJsonEscape()`) |
| `firmware/src/wifi_web.h` | Web UI 升级卡片 + JS 一键/手动上传 |
| `firmware/src/terminal_link.h` | C3 OTA 回复(OTAR/OTAN/OTAOK/OTAFAIL)分发 |
| `firmware/src/ebike_bsd.ino` | `wifiAutoOffTick()` WiFi 自动关状态机(封装, OTA 忙时跳过) |
| `terminal/src/uart_link.h` | C3 OTA 接收(Update.write + ACK/NACK + 回滚保护 + size/hex 校验) |
| `terminal/src/c3_terminal.ino` | C3 升级中屏幕提示 + 回滚确认 + 15s 超时保护 |
| `terminal/partitions_ota_8M.csv` | C3 OTA 双槽分区表 |
| `.github/workflows/build-release.yml` | 自动构建 + 版本一致性校验 + 发布 manifest 到 gh-pages |

## 七、实车测试结果 (2026-07-12)

### 主控 ESP32 OTA — ✅ 全部通过

| 测试 | 结果 | 说明 |
|---|---|---|
| 版本号 + 启动 | ✅ | V2.8 串口横幅, SPIFFS/WiFi/Web 全部正常 |
| OTA 上传升级 | ✅ | ota_0→ota_1, 996KB / 7.4s, SHA-256 校验通过 |
| 自动回滚 | ✅ | 坏固件(setup死循环)→3 次重启→boot guard 触发→回 ota_0 |
| WiFi 自动关 + OTA 共存 | ✅ | 日常 30s 省电不变, OTA 期间保持稳定 |

### 分发链路 — ✅ 通过

| 测试 | 结果 | 说明 |
|---|---|---|
| GitHub Action 构建 | ✅ | tag v2.8 触发, 主控+C3 固件 + manifest.json 发布 |
| jsDelivr CDN | ✅ | manifest.json HTTP 200, 内容完整 |
| 手机网页云端检查 | ✅ | 显示"云端最新 V2.8" |

### C3 终端 OTA — ✅ 全部通过

| 测试 | 结果 | 说明 |
|---|---|---|
| 分区表重刷 | ✅ | bootloader 识别 ota_0/ota_1/otadata 双槽 |
| OTA 协议接收 | ✅ | 修复解析 bug 后, OTAR 持续递增无 NACK |
| 屏幕进度显示 | ✅ | 进度条正常更新 + 固件版本号显示 |
| 完整文件传输 | ✅ | 424KB / 3316 块, 零 NACK, Update.end 校验通过, 重启切新槽 |
| OTA 中断超时保护 | ✅ | 主控崩溃后 C3 15s 自动退出升级模式, 恢复通信 |

## 八、实车测试中发现并修复的真实问题

> 实车测试总共发现 8 个问题, 全部修复. 按发现顺序记录, 每条都有现象/根因/修复.

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

### 问题 4: OTA 中断后 C3 永久卡死在升级界面

**现象**: 主控转发期间崩溃, C3 的 `c3OtaProgress.active` 永远为 true, loop 跳过
`netLink.update()` → 不再处理 `$S` 帧 → C3 永远显示"主控离线", 无法自愈.

**根因**: OTA 进行时 loop 直接 `return`, 没有超时退出机制.

**修复** (`terminal/src/c3_terminal.ino` + `uart_link.h`):
- `C3OtaProgress` 加 `lastChunkMs` 记录最后收块时间
- loop 加 15 秒超时: 超时则自动退出 OTA 模式, 恢复正常通信

### 问题 5: c3_staged_version 从未赋值 (代码审查)

**现象**: `$OTAB` 帧的 version 字段永远为空, C3 屏幕显示"目标:"空白.

**根因**: `otaStatus.c3_staged_version` 声明了但无任何赋值点.

**修复** (`firmware/src/ota_manager.h`): C3 上传完成时设默认值 `size:<字节数>`.

### 问题 6: JSON 错误字符串未转义 (代码审查)

**现象**: error 字符串含 `"` 或 `\` 时破坏 JSON 结构, 前端 `JSON.parse` 失败.

**根因**: 4 处 JSON 输出直接拼接动态字符串, 无转义.

**修复**: 新增可复用 `otaJsonEscape()` 函数, 统一转义 4 处 (otaMainUploadDone /
otaC3UploadDone / otaStatusHandler 主控+C3).

### 问题 7: WiFi 30s 自动关与 OTA 转发冲突

**现象**: 恢复 WiFi 自动关后, C3 转发 (10 分钟) 期间若手机断开, 30s 后 WiFi 被关.

**根因**: 自动关逻辑只看 `sta_num`, 不知道 OTA 在进行中.

**修复** (长期可维护性重构):
- `otaIsBusy()` 用**黑名单** (只排除终态 IDLE/FAILED/SUCCESS, 其余自动算忙),
  新增 OTA 状态无需改本函数
- WiFi 自动关封装成独立函数 `wifiAutoOffTick()`, 从 loop 移出 ~25 行
- OTA 忙时直接 `return` 跳过自动关, 不再重载 `g_wifi_idle_since` 语义

### 问题 8: SPIFFS 频繁 open/close + hex/size 校验缺失 (代码审查)

**修复**:
- M1: C3 固件上传改用持久 `static File` 跨回调复用, 避免每 chunk open/close
- M2: C3 `handleOtaChunk` 加 `hexLen < 2` 拒绝, 防 `Update.write(_,0)`
- M3: GitHub Action 构建前校验 `FW_VERSION` 与 tag 一致, 防发错版本
- M4: C3 `handleOtaBegin` 用 `esp_ota_get_next_update_partition` 动态获取槽大小校验

## 九、C3 OTA 完整传输 (已解决)

C3 OTA 完整传输在第二轮实车测试中**成功**:
- 主控转发 424KB / 3316 块, **零 NACK**, 全部 ACK
- C3 收到 `$OTAE` 后 `Update.end(true)` 校验通过 → 重启切到新槽
- 屏幕进度条正常更新, 升级后恢复正常显示

**关键**: 完整传输需要稳定的供电环境. 首轮测试中主控在转发中途因 USB 供电不足
触发 BROWNOUT 重启 (诊断: `[RST] 上次重启原因: 上电复位`). 换稳定供电后成功.

## 十、设计要点 (长期可维护性)

OTA 功能在代码审查后做了重构, 遵循三个原则:

1. **开闭原则**: `otaIsBusy()` 用黑名单, 新增 OTA 中间状态自动算忙, 无需改既有代码
2. **模块解耦**: WiFi 逻辑只调用 `otaIsBusy()` 布尔接口, 不了解 OTA 内部状态枚举;
   OTA 模块不关心 WiFi 如何实现保活
3. **防呆校验**: GitHub Action 构建前校验版本一致性, 忘了同步源码版本号会 fail;
   C3 端动态获取分区大小校验, 分区表变了自适应



