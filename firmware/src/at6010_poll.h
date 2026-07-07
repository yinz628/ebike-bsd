// ============================================================
//  at6010_poll.h — AT6010 CMD 0x30 轮询获取完整检测信息
//  返回 fmcw_det_info_t (含 range/angle/velocity)
// ============================================================
#ifndef AT6010_POLL_H
#define AT6010_POLL_H

#include <Arduino.h>

// CMD 0x30: 获取雷达感应信息 (完整 fmcw_det_info_t)
// 结构体: is_detected(1) + det_result(1) + range_val(2) + angle_val(2) + velo_val(2) + reserved[6] + rb_conf(1) + angle_conf(1) + frame_idx(4) = 20 bytes

typedef struct {
    uint8_t  is_detected;     // 是否检测到目标
    uint8_t  det_result;      // 0x01=靠近 0x02=远离 0x04=运动 0x08=微动 0x10=呼吸
    uint16_t range_val;       // 距离, mm
    int16_t  angle_val;       // 角度, 1°单位 (signed)
    int16_t  velo_val;        // 速度 (协议标记为预留, 可能恒为0)
    // 后面还有 reserved[6], rb_conf, angle_conf, frame_idx — 我们不需要
} __attribute__((packed)) FMCWDetInfo;

// ============ 发送 CMD 0x30 并解析响应 ============
// 返回 true=成功解析, 结果写入 info
// _serial: 雷达 HardwareSerial 指针
static bool pollAT6010_DetInfo(HardwareSerial *_serial, FMCWDetInfo *info) {
    if (!_serial || !info) return false;
    
    // 发送 CMD 0x30 (无参数)
    // 帧: 58 30 00 88 00 (5 bytes, 2-byte checksum)
    uint8_t cmd[] = {0x58, 0x30, 0x00, 0x88, 0x00};
    _serial->write(cmd, 5);
    
    // 等待响应 (最多 100ms)
    unsigned long t0 = millis();
    uint8_t rx_buf[32];
    int rx_len = 0;
    
    while (millis() - t0 < 100) {
        while (_serial->available() && rx_len < 32) {
            rx_buf[rx_len++] = _serial->read();
        }
        if (rx_len >= 5) break;  // 至少收到 HEAD + CMD + LEN
    }
    
    if (rx_len < 5) return false;
    
    // 解析响应: 59 30 14 [20 bytes] [2B csum]
    if (rx_buf[0] != 0x59) return false;   // HEAD
    if (rx_buf[1] != 0x30) return false;   // CMD echo
    if (rx_buf[2] != 0x14) return false;   // LEN = 20
    
    size_t expected = 3 + 20 + 2;  // HEAD(1)+CMD(1)+LEN(1) + data(20) + csum(2)
    if (rx_len < expected) return false;
    
    // 校验和 (2 bytes LE)
    uint16_t calc_sum = 0;
    for (int i = 0; i < 3 + 20; i++) calc_sum += rx_buf[i];
    uint16_t rx_sum = rx_buf[23] | (rx_buf[24] << 8);
    if (calc_sum != rx_sum) return false;
    
    // 解析结构体
    uint8_t *data = rx_buf + 3;  // skip HEAD+CMD+LEN
    info->is_detected = data[0];
    info->det_result  = data[1];
    info->range_val   = data[2] | (data[3] << 8);
    info->angle_val   = data[4] | (data[5] << 8);
    info->velo_val    = data[6] | (data[7] << 8);
    
    return true;
}

#endif
