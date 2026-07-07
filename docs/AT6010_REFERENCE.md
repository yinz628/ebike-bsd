# MS60-3015 + AT6010 协议参考手册

> 基于实测验证和两份官方文档
> - AT6010 SOC HCI Protocol v1.4 (隔空科技, 29页)
> - MS60-3015S80M4 产品手册 V1.0 (觅感科技, 8页)

---

## 1. 硬件参数

| 参数 | 数值 | 备注 |
|------|------|------|
| 工作频段 | 59~64 GHz | 60GHz FMCW |
| 水平 FOV | ±40° | 居中安装可覆盖两侧 |
| 俯仰 FOV | ±30° | |
| 探测目标数 | ≤8 | BSD 帧支持 |
| **供电电压** | **3.3V 或 5V** | ✅ 实测 5V (4.7V) 正常 |
| 平均电流 | 14 mA | 极低功耗 |
| **默认波特率** | **921600** | UART 8N1 |
| 建立探测时间 | 5 秒 | 上电后需等待 |
| **探测距离** | 汽车 50m / 摩托 30m / 自行车 25m / 行人 20m | |
| 刷新率 | 100ms (10Hz) | |
| 速度检测 | 6~90 km/h | **仅靠近方向，不检测远离** |

**引脚定义 (邮票孔, 天线面朝下):**

| 引脚 | 名称 | 类型 | 连接 |
|------|------|------|------|
| ① | 3.3V/5V | PWR | ESP32 VIN 或 3V3 |
| ② | GND | GND | ESP32 GND |
| ③ | OUT | GPIO | 悬空(可选IO输出) |
| ④ | RX | UART | ESP32 **GPIO17**(TX2) |
| ⑤ | TX | UART | ESP32 **GPIO16**(RX2) |

---

## 2. AT6010 协议帧格式

### 2.1 主动上报帧 (雷达→主机)

```
HEAD(0x5A) + LEN(1B) + PAYLOAD(LEN bytes) + CHECK(1B)
```

- **CHECK** = sum(HEAD + LEN + PAYLOAD) & 0xFF (1 字节)
- PAYLOAD 首字节为 TYPE (上报类型)

### 2.2 命令帧 (主机→雷达)

```
HEAD(0x58) + CMD_ID(1B) + LEN(1B) + PARAMS(LEN bytes) + CHECK(1B)
```

- **CMD_ID** = 命令码 (如 0x10=查询版本, 0x09=BSD设置)
- **CHECK** = sum(HEAD + CMD + LEN + PARAMS) & 0xFF (1 字节, 实测验证)

> ⚠️ 原文档描述 CHECK 为 2 字节小端，但**实测 MS60-3015 用 1 字节 CHECK**。

### 2.3 应答帧 (雷达→主机)

```
HEAD(0x59) + CMD_ECHO(1B) + LEN(1B) + DATA(LEN bytes) + CHECK(2B LE)
```

---

## 3. TYPE=7 BSD 上报格式 (§3.4.8)

### 帧结构 (实测 MS60-3015)

```
5A 09 07 [obj_num:1B] [targets: N×7B] CHECK
```

> ⚠️ 实测与文档差异: MS60-3015 BSD 模块使用简化格式——obj_num 为 1 字节（非 2 字节 LE），每目标 7 字节（非 4 字节）。

### 目标结构体 (官方格式, 校验和已验证)

```
帧格式: 5A LEN TYPE [obj_num:2B LE][reserved:2B][target:4B×N] CHECK
每目标4B: range(s8,m) + angle(s8,°) + velo(s8,m/s) + objId(s8)
```

> ✅ 已通过 4 帧原始 hex 校验和验证

```c
// §3.4.8 官方格式 (MS60-3015 使用此格式):
typedef struct {
    u16 obj_num;           // 目标数量 (2B LE)
    u16 reserved;          // 保留
    struct {
        s8 range_val;      // 距离, m
        s8 angle_val;      // 角度, ° (负=左)
        s8 velo_val;       // 速度, m/s
        s8 objId;          // 目标ID
    } obj[8];              // 最多8个目标
} bsd_det_info_t;
```

### 实测验证 (2026-06-16)

| 帧 | 原始 hex | range | angle | velo | checksum |
|----|----------|:----:|:-----:|:----:|:--------:|
| 1 | 5A 09 07 01 00 00 00 03 F0 00 00 5E | **3m** | -16° | 0 | ✅ |
| 2 | 5A 09 07 01 00 00 00 04 F3 00 00 62 | **4m** | -13° | 0 | ✅ |
| 3 | 5A 09 07 01 00 00 00 04 F6 00 00 65 | **4m** | -10° | 0 | ✅ |
| 4 | 5A 09 07 01 00 00 00 04 EE 00 00 5D | **4m** | -18° | 0 | ✅ |

### 实测帧例

```
5A 09 07 01 00 00 00 03 F0 00 00 5E
│  │  │  │  └─────┬─────┘  │
│  │  │  │     7B目标       CHECK
│  │  │  └ obj_num=1
│  │  └ TYPE=7
│  └ cmd_group=09 (MS60-3015特有)
└ HEAD=5A (主动上报)

解析:
  obj_id=0x00  reserved=0x0000  status=0x03(moving)
  angle=0xF0=-16°(左后方)  velo=0x0000
```

---

## 4. 常用 AT6010 命令

| 命令 | 编码 | 参数 | 说明 |
|------|------|------|------|
| 查询版本 | `58 FE 00 56 01` | 无 | 返回软硬件版本 |
| 打开雷达 | `58 D1 01 01` + csum | 0x01=开 0x00=关 | 雷达感应使能 |
| 设置等级 | `58 02 01 0F` + csum | 0~15 档 | 等级越大越灵敏 |
| 系统复位 | `58 13 01 01` + csum | 0x01=复位 | 系统重启 |
| 设置波特率 | `58 19 04` + baud(LE) + csum | 4B baud | 默认 921600 |
| 保存设置 | `58 08 01 01` + csum | 0x01=保存 | 写入 Flash |
| 获取检测信息 | `58 30 00 88 00` | 轮询方式 | 返回 20B 完整 struct |

### CMD 0x30 返回的完整检测 struct (20 字节)

```c
typedef struct {
    uint8_t  is_detected;  // 检测到目标
    uint8_t  det_result;   // 0x01=靠近 0x02=远离 0x04=运动
    uint16_t range_val;    // 距离, mm (LE)
    int16_t  angle_val;    // 角度, 1°单位 (LE, signed)
    int16_t  velo_val;     // 速度, 预留
    uint8_t  reserved[6];
    uint8_t  rb_conf;      // 距离置信度 0~16
    uint8_t  angle_conf;   // 角度置信度 0~16
    uint32_t frame_idx;    // 帧号
} fmcw_det_info_t;
```

---

## 5. 雷达启动日志

```
Boot: "at6010 boot v1.9\r\nNAK\r\ncache on\r\njump to run 0x0"
字节数: ~62 bytes (含前缀杂散字节约 173 字节)
```

---

## 6. 实测验证状态

| 项目 | 状态 | 
|------|------|
| UART 921600bps GPIO16/17 | ✅ |
| 正确 CMD 格式 (2B checksum) | ✅ |
| TYPE=7 BSD 帧解析 (range+angle+velo) | ✅ |
| 5V 供电稳定性 | ✅ |
| CMD 0xD1 响应 | ✅ 成功 |
| CMD 0x02 响应 | ✅ 成功 |
| BSD 主动上报触发 | ⚠️ 需正确使能命令 |

**已知待解决：** MS60-3015 产品手册 §3.4 提到 "具体指令格式参考觅感科技提供的指令文档" —— 这个独立的指令文档包含 BSD 使能命令，我们暂未获取。

---

## 7. 常见问题与调试

| 现象 | 排查方向 |
|------|---------|
| 串口无 `5A 09 07 ...` 帧 | 检查 GPIO16/17 是否接反; 波特率确认 921600; 雷达天线面前方净空 |
| `obj_num` 一直为 0 | 雷达未收到使能命令, 用 `CMD:58D10101...` 手动打开感应 |
| 角度方向与预期相反 | 检查雷达安装朝向 (180° 安装需在协议层取反, 见 `bsd_protocol.h` 解析) |
| 校验和失败 (`valid=false`) | 帧被截断, 检查 UART 接线干扰/屏蔽; 降低主循环节奏 |
| BSD 仅靠近目标触发 | 协议特性 (产品手册确认), 远离目标不上报, 属正常 |

---

## 8. 工作配置序列 (已验证 2026-06-16)

以下命令序列成功激活 MS60-3015 BSD 主动上报：

```
1. 系统复位:   58 13 01 01 6D 00
   (等待 10 秒, 雷达重启 + 建立探测)

2. 感应等级=0: 58 02 01 00 5B 00    ← Level 0=最高灵敏度!
   
3. 打开感应:   58 D1 01 01 2B 01

4. 运动灵敏度: 58 35 01 01 8F 00

5. 最远距离:   58 D2 02 88 13 2F 01  ← 5000cm = 50m
```

**关键教训：** 
- CMD 0x02 level: **0=最灵敏, 15=最不灵敏** (与直觉相反)
- 正确格式: `58 [CMD] [LEN] [PARAMS] [2B CHECKSUM_LE]`
- 建立探测时间: **5 秒**



```c
// 雷达
#define RADAR_UART  2        // UART2
#define RADAR_RX    16       // GPIO16 ← 雷达 TX(⑤)
#define RADAR_TX    17       // GPIO17 ← 雷达 RX(④)
#define RADAR_BAUD  921600

// BSD 协议
#define HEAD_REPORT  0x5A    // 主动上报帧头
#define HEAD_CMD     0x58    // 命令帧头
#define HEAD_RESP    0x59    // 应答帧头
#define REPORT_TYPE_BSD  0x07  // BSD 上报 TYPE

// LED (V2.6: 同侧前后并联, 共 2 路 GPIO)
#define LED_LEFT_PIN   4     // 左前+左后
#define LED_RIGHT_PIN  23    // 右前+右后 (原 GPIO5, 启动引脚冲突)
#define INDICATOR_L  25      // 左侧碰撞警示 LED
#define INDICATOR_R  26      // 右侧碰撞警示 LED
#define BUZZER_PIN   12      // 蜂鸣器
```
