// ============================================================
//  term_protocol.h - 主控 ESP32 ↔ C3 终端 共享协议定义
//
//  本头是两端通信协议的**单一真源**:
//    - UART 链路波特率
//    - OTA 分块大小
//    - CRC16 校验算法
//
//  两端 PlatformIO 工程通过 build_flags = -I../protocol 引用本头.
//  改协议只需改这一处, 两端重新编译即同步, 杜绝"改一端忘另一端".
//
//  ⚠ 本头不依赖 Arduino/ESP-IDF, 可被任意 C++ 翻译单元安全 include.
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// ==== UART 链路波特率 (主控 terminal_link.h 和 C3 uart_link.h 必须一致) ====
#define TERM_BAUD 115200

// ==== OTA 分块大小 (主控 ota_manager.h 发送 和 C3 uart_link.h 接收必须一致) ====
//   裸字节数; 编码成 hex 后翻倍 (128B → 256 hex 字符).
//   帧缓冲大小 = 帧头(~16) + 2*OTA_BLOCK_BYTES + 帧尾(~8) ≈ 288, 两端均放大到 320.
#define OTA_BLOCK_BYTES 128

// ==== CRC16-CCITT (OTA 分块校验, 两端算法必须逐字节一致) ====
//   多项式 0x1021, 初值 0xFFFF. 用于 $OTAC 帧的 crc16 字段.
inline uint16_t termCrc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}
