# EasyEDA 手动建原理图 — V2.7 版

> 打开 [easyeda.com/editor](https://easyeda.com/editor) → 新建工程 → 新建原理图

## 步骤 1: 放置元件

| 搜索关键词 | 数量 | 对应设计 | 备注 |
|-----------|:--:|------|------|
| `XL7015` | 1 | U1 | DC-DC 48V→12V |
| `LM2596` | 1 | U2 | DC-DC 12V→5V |
| `IRLZ44N` | **2** | Q1-Q2 | TO-220, 仅需2个(同侧并联) |
| `Header 2P 5.08` | 2 | J1,J2 | 48V输入/12V输出 |
| `Header 5P 2.54` | 1 | J3 | 雷达 |
| `Header 3P 2.54` | 1 | J4 | 蜂鸣器 |
| `Header 4P 2.54` | 2 | J5,J6 | 开关 + C3面板 |
| `Header 4P 5.08` | 1 | J7 | 转向灯输出 |
| `Resistor 100` | 2 | R1,R2 | MOSFET 栅极限流 |
| `Resistor 10k` | 2 | RPD1,RPD2 | 栅极下拉 |
| `Resistor 220` | 2 | R5,R6 | LED限流 |
| `LED 5mm Red` | 2 | D1-D2 | RCW指示灯 |
| `Fuse Holder` | 1 | F1 | 5×20 保险丝座 |
| `ESP32 DevKitC` | 1 | ESP32 | 或 2×19排母 |

## 步骤 2: 连线 (V2.7 网表)

### 电源
```
J1(+)─F1─XL7015 IN+    XL7015 OUT+─+12V─J2(+)─LM2596 IN+
               IN-─GND            OUT-─GND    OUT+─+5V
                                              OUT-─GND
```

### 4路转向灯 → 2路MOSFET (同侧并联)
```
ESP32 GPIO4─┬─100Ω(R1)──Q1.G   Q1.D─┬──J7.1(左灯-)
            │                   │    │
            └─10kΩ(RPD1)─GND   Q1.S─GND
                                     J7.3─+12V (所有灯正极)

ESP32 GPIO23─┬─100Ω(R2)──Q2.G   Q2.D─┬──J7.2(右灯-)
             │                   │    │
             └─10kΩ(RPD2)─GND   Q2.S─GND
                                     J7.4─+12V (所有灯正极)
```
> 左前灯(-) + 左后灯(-) → 一起接 J7.1
> 右前灯(-) + 右后灯(-) → 一起接 J7.2

### 雷达 (J3)
```
J3.1(VCC)─+5V    J3.2(GND)─GND    J3.3(OUT)─悬空
J3.4(RX)─GPIO17  J3.5(TX)─GPIO16
```

### 蜂鸣器 (J4)
```
J4.1(VCC)─+5V    J4.2(GND)─GND    J4.3(I/O)─GPIO12
```

### C3 显示面板 (J6)
```
J6.1─GPIO19 (U1RX)   J6.2─GPIO18 (U1TX)
J6.3─GND             J6.4─+5V
```

### 开关 (J5)
```
J5.1(SW_L)─GPIO13    J5.2(SW_R)─GPIO14
J5.3─悬空             J5.4(GND)─GND
```

### RCW LED
```
GPIO25─220Ω(R5)─D1(+)─D1(-)─GND
GPIO26─220Ω(R6)─D2(+)─D2(-)─GND
```

## 步骤 3: 转换为 PCB

1. 原理图画完 → "设计" → "转换为PCB"
2. 按 PCB_DESIGN.md 布局摆放
3. 自动布线 → 检查 → 手动调整
4. 导出 Gerber → 发 JLCPCB

## 接地注意
**所有 GND 必须共地！** 48V、12V、5V、ESP32、MOSFET S极全部连在一起。
PCB 底层铺完整铜皮作为地平面。
