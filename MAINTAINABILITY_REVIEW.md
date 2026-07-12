# ebike-bsd 长期可维护性审查报告

> 审查日期: 2026-07-12
> 审查范围: 全项目 (firmware/ 主控 ESP32 + terminal/ C3 终端 + 跨模块协议)
> 审查方法: 三维度并行深入分析, 每条问题附文件:行号 + 代码证据

---

## 一、总体评价

项目功能完整、实车验证通过,代码注释质量整体偏高(多解释"为什么")。但作为长期维护的项目,存在以下**结构性可维护性债务**:

- **架构层面**: header-only 风格 + 全局变量 + include 顺序耦合,形成脆弱的编译依赖链
- **模块层面**: `loop()` 上帝函数、职责膨胀的类、视图层无抽象基类
- **跨模块层面**: 主控↔C3 的协议完全靠手工同步(双写),无编译期保护,改动一端忘了另一端只会运行时出错

**代码规模**: 主控 ~3356 行(11 文件), C3 ~1775 行(7 文件), 跨模块协议定义散落 6+ 文件

---

## 二、问题分级汇总

| 严重度 | 数量 | 说明 |
|---|---|---|
| 🔴 Critical | 4 | 阻碍安全重构, 改动即可能编译失败或运行时错乱 |
| 🟠 High | 9 | 显著增加维护成本, 每次改动都要小心连锁反应 |
| 🟡 Medium | 8 | 增加阅读理解负担, 偶发 bug 温床 |
| 🟢 Low | 5 | 代码异味, 建议改善但不紧急 |

---

## 三、Critical 问题 (必须优先处理)

### C1. Include 顺序构成编译级隐式依赖

**位置**: `firmware/src/ebike_bsd.ino:67-85`

`.ino` 里 `#include` 语句穿插在全局变量声明和 typedef 之间,形成 5 条强制的顺序约束(任一破坏即编译失败):

```
config_store.h → bsd_protocol.h/ms60_radar.h  (inline 函数依赖 config 全局对象)
TurnState_t typedef(.ino:74) → wifi_web.h/terminal_link.h (extern 引用)
全局对象 radar/config(.ino:68,70) → wifi_web.h/terminal_link.h (extern)
ota_manager.h → wifi_web.h (initWebServer 调 otaRegisterRoutes)
g_wifi_running/g_wifi_idle_since(.ino:105) → ota_manager.h (extern)
```

代码用注释承认了这种脆弱性(`.ino:81-82`),但没结构性修复。

**根本问题**: `TurnState_t` 这个跨文件共享的类型定义在 `.ino` 里而非头文件;`ota_manager.h` ↔ `terminal_link.h` 形成隐式双向调用(靠 include 顺序硬凑)。

**修复方向**: 把跨文件共享的类型/extern 集中到一个 `types.h` 或各自模块头;头文件间用 `#include` 显式声明依赖,而非靠 `.ino` 的 include 顺序。

---

### C2. 头文件里定义非 inline 全局对象 — 多翻译单元即链接失败

**位置**:
- `firmware/src/wifi_web.h:20` — `AsyncWebServer server(80);` (定义,非 extern)
- `firmware/src/ota_manager.h:85` — `OtaStatus otaStatus;`
- `firmware/src/terminal_link.h:269` — `TerminalLink termLink;`
- `terminal/src/uart_link.h:508` — `C3OtaProgress c3OtaProgress;`

这些全局对象**定义在头文件里**,靠"整个项目只有一个 .ino 翻译单元"才侥幸能编过。一旦未来拆出 `.cpp` 或加单元测试,立即重复定义链接错误。注释(`ota_manager.h:84`)自己承认"本头仅被包含一次,故在此定义"——这是对编译模型的妥协,不是设计。

**修复方向**: 头文件里只 `extern` 声明,在 `.ino`(或新建 `.cpp`)里定义。

---

### C3. $CFG 帧 15 个值的顺序是纯注释契约,无程序性约束

**位置** (三处手工同步):
- `firmware/src/terminal_link.h:98-113` (主控发送, snprintf 15 个 `%d`)
- `terminal/src/uart_link.h:140-143` (C3 接收, 注释"顺序与主控一致")
- `terminal/src/config_view.h:28-30` (C3 视图, 再写一遍同样注释)

若主控在 `snprintf` 插入一个新字段到第 3 位,忘了改 C3 —— C3 的 `cfg[12]` 读到错位值,`det_range` 被当 `sensitivity`,用户调参后回写主控污染 NVS。**无任何边界检查或序号校验**,且注释里的数字已经不一致(主控说"15个",C3 说"12个",见 `uart_link.h:329`)。

**同类问题**: `$S` 帧字段顺序也是两端各自硬编码(`terminal_link.h:238-247` vs `uart_link.h:187-195`),帧里无版本号字段。

**修复方向**: 抽共享协议头(见 C4);或帧加字段计数前缀(`$CFG,15,v0,...`),C3 按计数解析,不匹配则丢弃告警。

---

### C4. OTA 协议常量和 CRC 算法两端复制粘贴,改一端 OTA 静默失败

**位置**:
- `OTA_BLOCK_BYTES 128`: `firmware/src/ota_manager.h:43` + `terminal/src/uart_link.h:33` (各定义一次)
- CRC16 算法: `ota_manager.h:176-185` + `uart_link.h:48-57` (函数体逐字节相同)
- `TERM_BAUD 115200`: `terminal_link.h:43` + `uart_link.h:23`
- 发送/接收缓冲大小 320: `ota_manager.h:502` + `uart_link.h:215` (各自手算)

若有人改主控 `OTA_BLOCK_BYTES` 为 256,C3 仍按 128 解析 → 缓冲区溢出 + CRC 永久不匹配 → OTA 100% 失败。两个工程独立构建,**编译不会报错**,故障只在运行时表现为"NACK 重传循环"。

**修复方向**: 抽出一个**共享协议头**(仓库根 `protocol/term_protocol.h`),两端通过 `lib_extra_dirs` 引用,集中定义所有协议常量、CRC 函数、字段索引枚举。改一处两端都变。

---

## 四、High 问题 (显著增加维护成本)

### H1. `loop()` 上帝函数 — 135 行, 10+ 职责, 隐式数据流

**位置**: `firmware/src/ebike_bsd.ino:226-360`

`loop()` 承担 WiFi 自动关、读开关、读雷达、雷达重连状态机、串口命令解析(7 种)、指示灯重置、RCW 检测、转向辅助、LED、蜂鸣器、C3 链路、OTA 等 10+ 职责。

**最严重的隐患 — 隐式优先级覆盖**: 各 `update*` 函数靠全局变量传参,且有覆盖语义:
- `updateRCW()` 设 `buzzer_mode = 2`(`.ino:544`)
- `updateTurnAssist()` 随后设 `buzzer_mode = 3`(`.ino:466`),**覆盖**前者

谁赢取决于 loop 调用顺序(`.ino:323` 在 `.ino:327` 前)。优先级隐含在调用顺序里,无注释或类型系统保证。`.ino:471` 的 `!rcw_l_active` 检查是开发者对抗覆盖的补丁。

---

### H2. 14+ 跨文件 extern 全局态, 模块边界形同虚设

主控有 14 个全局对象/变量被 3+ 文件 extern 共享: `radar`/`config`/`server`/`otaStatus`/`termLink`/`turn_state`/`buzzer_mode`/`ind_left_mode`/`ind_right_mode`/`bsd_l/r_active`/`rcw_l/r_active`/`g_wifi_running`/`g_wifi_idle_since`...

`buzzer_mode` 被 `.ino` 写 12 处、`terminal_link.h:240` 读、`wifi_web.h:51,53` 读,无任何封装。无法对 `updateRCW` 做单元测试(它依赖十几个全局态并产生副作用)。

`config` 对象的 extern 在 `config_store.h:239` 自身定义,导致 `ms60_radar.h:87,88` 直接读 `config.radar.sensitivity` —— 雷达驱动类本应自包含,现在硬耦合到全局 config。

---

### H3. 3 个完整废弃头文件 (306 行死代码)

| 文件 | 行数 | 状态 | 证据 |
|---|---|---|---|
| `oled_display.h` | 147 | **完全废弃**, 版本号还停留在 V2.7 | grep 零外部引用; `.ino:48-49` 注释"主控不接 OLED" |
| `at6010_poll.h` | 72 | **完全废弃** | `pollAT6010`/`FMCWDetInfo` 零调用 |
| `at6010_protocol.h` | 87 | **废弃 + 宏重复定义 bug** | `:30` 和 `:43` 重复 `#define AT6010_CMD_GET_DET_INFO 0x30` |

`bsd_protocol.h:149-177` 还有半废弃段(`BSD_CMD_GROUP 0x09 // 旧代码使用，实际已废弃`)。

**修复方向**: 直接删除 3 个文件,清理 `bsd_protocol.h` 废弃段。零风险、立即减负。

---

### H4. 视图层无共同基类 — 加页面是散弹枪修改

**位置**: `terminal/src/`

`RadarView`/`StatusView`/`ConfigView` 三个类完全独立,无继承/虚函数接口。`markDirty()` 三个类各写一遍(且 `ConfigView` 字段名已变为 `_needsDraw`)。

**加第 4 个页面要改 5 处**:
1. `c3_terminal.ino:94-102` `updatePages()` 加 `totalPages++`
2. `c3_terminal.ino:188-204` `loop()` 的 `switch(currentPage)` 加 `case 3:`
3. `c3_terminal.ino:268-278` `markCurrentDirty()` 加分支
4. `c3_terminal.ino:329-333` `handleTouch()` 加分支
5. 全局对象声明 + `#ifdef ENABLE_XXX_VIEW` 块

---

### H5. `uart_link.h` 职责膨胀 — 508 行, 5 类职责

`uart_link.h` 混合了: (a) $S 状态帧解析, (b) $CFG 配置帧解析, (c) 触摸命令发送, (d) OTA 接收协议(`handleOtaBegin/Chunk/End` 137 行), (e) 回滚保护(`c3OtaBootGuardBegin` + `verifyRollbackLater`)。

`UartLink::update()`(`:266-309`)的同一个循环同时处理 `$S`/`$CFG`/`$OTAB`/`$OTAC`/`$OTAE` 五种帧。OTA 改动(如调块大小)必须动 `update()` 主循环,**回归风险波及日常通信**。`handleOtaEnd()` 内含 `delay(500)+ESP.restart()`,藏在通用 update 里。

---

### H6. OTA 代码与日常通信强耦合

`UartLink` 类一个实例同时管通信和 OTA。`_rx_buf[320]` 容量按 OTA 帧设计(注释 `:213-214` 承认),日常帧本无需这么大。`_ota_block[128]` 是类成员,OTA 结束后缓冲残留。OTA 失败路径若 `Update.printError` 卡住,整个 update() 阻塞,日常 $S 也收不到。

---

### H7. `c3OtaProgress` 跨模块全局可变状态 — 封装破裂

`uart_link.h:45` extern + `:508` 定义。**6 处写入分散两文件**: `uart_link.h:388,409-414,477-479,492,502` + **`c3_terminal.ino:171` 从外部直接翻转 `c3OtaProgress.active = false`**,绕过 UartLink 任何不变量检查。

若 OTA 内部还 `_ota_active=true` 但外部把 `c3OtaProgress.active=false`,两者失配 —— 下一个 `$OTAC` 块到达时逻辑混乱。

---

### H8. 错误处理 4 套风格并存

| 风格 | 代表 | 问题 |
|---|---|---|
| Serial.println 后静默继续 | `.ino:203-205`(雷达失败后空转) | 系统在无雷达下空转,用户无感知 |
| 返回 bool 但调用方忽略 | `.ino:279 config.saveToNVS()` 忽略返回值 | 配置可能没存 |
| 返回 bool 且检查 | `ota_manager.h:261` | ✅ 最规范 |
| 静默 return | `ota_manager.h:141`、`terminal_link.h:121` | 错误被吞 |

**真实 bug 温床**: `wifi_web.h:478-479` —— `saveToNVS` 失败仅打印 WARN 但仍返回 HTTP 200 OK,**用户以为配置保存了实际没有**。

---

### H9. `$C` 命令 key 集合两端不对称, 无文档化

主控处理 18 个 key(`terminal_link.h:152-169`),C3 只发 14 个(`config_view.h:34-49`)。差 4 个角度参数(`rcw_lmin/lmax/rmin/rmax`)只能通过 WiFi Web 改,不能通过 C3 触摸屏改 —— **未文档化的功能不对称**。删主控的 `rcw_lmin` 分支不会编译报错,但 Web 端静默失效。

---

## 五、Medium 问题

### M1. 矛盾魔数 — `i < 8` vs `BSD_MAX_OBJECTS = 4` (潜在越界)

`ebike_bsd.ino:434,514,781,807` 用 `i < 8` 遍历雷达对象,但 `bsd_protocol.h:32` 的 `BSD_MAX_OBJECTS` 是 4,数组只有 4 槽。**按 8 遍历会越界读相邻内存**。这是双雷达时代的历史残留,现在是真实 bug 信号。

### M2. `WDT_TIMEOUT_S` 定义了不用

`.ino:22` 定义 `#define WDT_TIMEOUT_S 5`,但 `.ino:208`、`ota_manager.h:265,301` 全部硬编码 `esp_task_wdt_init(5, true)`。

### M3. config_view 触摸坐标 vs 绘制坐标不一致 (9px 错位 bug)

`config_view.h:174` 绘制 SAVE 按钮 `y=214`,`config_view.h:211` 命中检测 `y>=205` —— **命中区比可见按钮向上延伸 9px**,用户点按钮上方空白也会触发。根因是坐标重复定义且无单一真源。

### M4. 主题色 40+ 处重复硬编码

`color888(13,17,23)`(底色)、`color888(88,166,255)`(蓝)、`color888(248,81,73)`(红)、`color888(63,185,80)`(绿)、`color888(139,148,158)`(灰)在 4 个 C3 文件里出现 40+ 次。换主题色要逐处改。`lgfx_config.hpp` 已有 `NAV_BTN_W/H` 宏但没扩展到颜色。

### M5. 30+ 处恒开的 `#ifdef ENABLE_*` 开关

`c3_terminal.ino:56-60` 五个 `#define ENABLE_*` 全部启用,但全文散布 30+ 处条件编译。`switch(currentPage)` 每个 case 裹一层 `#ifdef`,嵌套到连续 `#endif #endif`。分阶段开发已完成(注释标 P2-P4 都过了),这些开关应清理为无条件代码。

### M6. 版本号三重定义

`FW_VERSION`(源码 #define) + `APP_VERSION`(platformio.ini -D) + manifest version(tag),三处手工同步。`FW_VERSION` 用 `#ifndef` 守卫意味着 `APP_VERSION` 实际不会覆盖它。GitHub Action 只校验主控 `FW_VERSION` == tag,不校验 C3、不校验 `APP_VERSION`。

### M7. `lib_deps` 用 `^` 不锁定

`ArduinoJson @ ^6.21.5` 允许 6.x.y 任意版本。ArduinoJson 6.x 内部 API 有过破坏性变化(StaticJsonDocument → JsonDocument)。无锁文件,新克隆每次构建可能得到不同版本。建议改 `=` 精确锁定 + 提交锁文件。

### M8. 注释含过时 TODO/版本号

- `.ino:55` `// TODO: updateTurnControl 尚未实现消抖` + `STATE_DEBOUNCE_MS 50` 死宏
- `oled_display.h:46` 硬编码 V2.7(当前 V2.8)
- `uart_link.h:329` 注释"12个值"实际 15 个

---

## 六、Low 问题

| # | 问题 | 位置 |
|---|---|---|
| L1 | 内嵌 300 行 HTML/JS 在 `wifi_web.h:75-447`,无法 lint/测试,CDN URL 硬编码仓库名 | `wifi_web.h:310-311` |
| L2 | 4 处 `StaticJsonDocument<4096>` 静态实例,每处 4KB | `config_store.h:169,187`、`wifi_web.h:36,61,472` |
| L3 | C3 TX/RX 引脚直接魔数 `19,18` 无 `#define` | `uart_link.h:236` |
| L4 | `radar_view.h:152-153` 同一作用域混用 `color888` 和 `color565` | — |
| L5 | `sendFactoryReset()`/`sendReset()` 零调用的遗留函数 | `uart_link.h:324-327` |

---

## 七、改进路线图 (按 ROI 排序)

### 第一阶段: 零风险清理 (立即收益, 不改行为)

| 动作 | 减负 | 风险 |
|---|---|---|
| 删除 `oled_display.h`/`at6010_poll.h`/`at6010_protocol.h` | -306 行死代码 | 零 |
| 清理 `bsd_protocol.h:149-177` 废弃段 | -28 行 | 零 |
| 修 `i < 8` → `i < BSD_MAX_OBJECTS`(M1) | 修越界 bug | 零 |
| 删 C3 的 `sendFactoryReset`/`loadDemoData`/DEMO 宏/恒开 ENABLE_* | -80 行噪声 | 零 |
| 修 config_view SAVE 按钮 9px 错位(M3) | 修 UI bug | 零 |

### 第二阶段: 结构性解耦 (中成本, 高收益)

| 动作 | 解决 | 方法 |
|---|---|---|
| 抽**共享协议头** `protocol/term_protocol.h` | C3/C4 双写 | 两端 `lib_extra_dirs` 引用, 集中定义常量/CRC/字段枚举 |
| 全局对象定义从头文件移到 .ino | C2 链接炸弹 | 头里改 extern, .ino 里定义 |
| `TurnState_t` 移入头文件 | C1 顺序耦合 | 新建 `types.h` 或放入 `bsd_protocol.h` |
| 抽主题色到 `lgfx_config.hpp` namespace | M4 | `namespace theme { constexpr auto BG = ...; }` |

### 第三阶段: 架构提升 (高成本, 长期收益)

| 动作 | 解决 | 方法 |
|---|---|---|
| `loop()` 拆分为状态机/调度器 | H1 上帝函数 | 各 update* 返回状态, 调度器按优先级合并 |
| 视图层抽 `ViewBase` 抽象基类 | H4 散弹枪 | 虚函数 `draw/markDirty/handleTouch`, 数组替 switch |
| `uart_link.h` 拆分 | H5/H6 职责膨胀 | 通信类 + OTA 接收类 + 回滚保护分离 |
| `c3OtaProgress` 收进类 + 访问器 | H7 封装破裂 | `otaReceiver.isActive()/getProgress()` 替全局 |
| 错误处理统一 | H8 风格混乱 | 定义项目级错误处理约定 + saveToNVS 失败返回正确 HTTP 码 |
| CI 加协议一致性校验 | C3/C4 运行时才发现 | 脚本提取两端字段/key/CRC 做 diff, 不一致 fail |
| lib_deps 精确锁定 + 锁文件 | M7 | `=` 替 `^` + `pio pkg install --lock` |

---

## 八、结论

项目的**功能实现是扎实的**(实车验证、安全机制完备、OTA 全链路打通),但**架构上积累了明显的维护债务**。最核心的两个结构性问题:

1. **header-only + 全局变量 + include 顺序耦合**(C1/C2/H2)—— 让"重构"变成危险操作,任何模块边界的调整都可能引发编译失败或链接错误。
2. **跨 MCU 协议纯靠注释双写**(C3/C4/H9)—— 改一端忘了另一端不会编译报错,只会在运行时表现为数据错位或 OTA 失败,极难定位。

建议按"零风险清理 → 共享协议头 → 全局对象解耦 → 架构提升"的顺序渐进重构。第一阶段的清理(删死代码、修越界 bug、修 UI 错位)可以立即做,立即见效。
