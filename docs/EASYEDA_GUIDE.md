# EasyEDA 手动建原理图 — 按步骤操作

> 打开 [easyeda.com/editor](https://easyeda.com/editor) → 新建工程 → 新建原理图

## 步骤 1: 放置元件

在左侧 **"常用库"** 搜索并放置以下元件:

| 搜索关键词 | 数量 | 对应设计 | 备注 |
|-----------|:--:|------|------|
| `XL7015` | 1 | U1 | 选 "DC-DC Power Module" |
| `LM2596` | 1 | U2 | 选 "DC-DC Step Down" |
| `IRLZ44N` | 4 | Q1-Q4 | 选 TO-220 封装 |
| `Header 2P 5.08` | 3 | J1,J2 | 接线端子 |
| `Header 5P 2.54` | 1 | J3 | 雷达 |
| `Header 3P 2.54` | 1 | J4 | 蜂鸣器 |
| `Header 4P 2.54` | 1 | J5 | 开关 |
| `Header 8P 5.08` | 1 | J6 | 转向灯输出(4路×2) |
| `Resistor 100` | 4 | R1-R4 | 选 0805 或 AXIAL-0.3 |
| `Resistor 220` | 2 | R5-R6 | |
| `LED 5mm Red` | 2 | D1-D2 | |
| `Fuse Holder` | 1 | F1 | 5×20 保险丝座 |
| `ESP32-DevKitC` | 1 | ESP32 | 或 `Header 15×2 Female` |

## 步骤 2: 连线 (按网表)

### 电源部分
```
J1(+)──F1──XL7015 IN+    XL7015 OUT+──+12V总线──J2(+)──LM2596 IN+
                 IN-──GND           OUT-──GND     OUT+──+5V总线
                                                       OUT-──GND
所有 GND 连在一起!
```

### 雷达 (J3)
```
J3.1(VCC)──+5V    J3.2(GND)──GND    J3.3(OUT)──悬空
J3.4(RX)──ESP32 GPIO17    J3.5(TX)──ESP32 GPIO16
```

### 4路转向灯 (每路相同)
```
ESP32 GPIO4──100Ω(R1)──IRLZ44N G(1脚)
                         D(2脚)──J6.1(接灯负极)
                         S(3脚)──GND
        +12V──J6.2(灯正极, 用户外接)

GPIO5──100Ω(R2)──Q2.G, D──J6.3, S──GND, J6.4←+12V
GPIO18──100Ω(R3)──Q3.G, D──J6.5, S──GND, J6.6←+12V
GPIO19──100Ω(R4)──Q4.G, D──J6.7, S──GND, J6.8←+12V
```

### RCW LED
```
ESP32 GPIO25──220Ω(R5)──LED D1(+)──D1(-)──GND
ESP32 GPIO26──220Ω(R6)──LED D2(+)──D2(-)──GND
```

### 蜂鸣器 (J4)
```
J4.1(VCC)──+5V    J4.2(GND)──GND    J4.3(I/O)──ESP32 GPIO12
```

### 开关 (J5)
```
J5.1(左)──ESP32 GPIO13
J5.2(右)──ESP32 GPIO14
J5.3(双闪)──ESP32 GPIO15
J5.4(GND)──GND
```

## 步骤 3: 转换为 PCB

1. 原理图画完 → 顶部菜单 "设计" → "转换为PCB"
2. 按 PCB_DESIGN.md 的布局建议摆放元件
3. 点 "自动布线" → 检查 → 手动调整
4. 导出 → Gerber → 发 JLCPCB

## 接地注意

**所有 GND 必须共地！** 48V输入地、12V输出地、5V输出地、ESP32地、MOSFET S极——全部连在一起。

PCB 上做**完整地平面**(底层铺铜)最优。
