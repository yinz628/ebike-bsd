// AT6010 原生协议命令 — 基于官方 HCI Protocol v1.4
// ============================================================
#ifndef AT6010_PROTOCOL_H
#define AT6010_PROTOCOL_H

#include <Arduino.h>

// ============ 帧常量 ============
#define AT6010_HEAD_CMD      0x58  // 主机→雷达 命令帧头
#define AT6010_HEAD_RESP     0x59  // 雷达→主机 应答帧头
#define AT6010_HEAD_REPORT   0x5A  // 雷达→主机 主动上报帧头

#define AT6010_CMD_GROUP_BASIC    0  // 基本命令组
#define AT6010_CMD_GROUP_RADAR    3  // 雷达配置组 (0xD0-0xDF)

// ============ 打包命令码: [group(3bits) | cmd(5bits)] ============
inline uint8_t AT6010_packCmd(uint8_t group, uint8_t cmd) {
    return ((group & 0x07) << 5) | (cmd & 0x1F);
}

// ============ 命令定义 (Group 0) ============
#define AT6010_CMD_SET_OUT_TIME     0x04
#define AT6010_CMD_GET_OUT_TIME     0x05
#define AT6010_CMD_SAVE_SETTINGS    0x08
#define AT6010_CMD_SYSTEM_RESET     0x13
#define AT6010_CMD_SET_BAUD         0x19
#define AT6010_CMD_QUERY_VERSION    0xFE

// ============ 雷达配置命令 (Group 3→0x60-0x7F) ============
#define AT6010_CMD_GET_DET_INFO     0x30  // 0x30 = [000][10000] = Group 0, CMD 0x10? No...
// 实际上 0x30 = 0011 0000 = Group 1? 让我重新算...

// 打包后的命令码:
// Group 0: 0x00-0x1F  (CMD = 0x00-0x1F)
// Group 1: 0x20-0x3F  (CMD = 0x00-0x1F)
// Group 2: 0x40-0x5F
// Group 3: 0x60-0x7F

// 协议中的命令码已打包:
#define AT6010_CMD_RADAR_LEVEL      0x02  // 设置雷达感应等级 (Group0 CMD2)
#define AT6010_CMD_GET_RADAR_LEVEL  0x03  // 获取雷达感应等级
#define AT6010_CMD_GET_ALGO_TYPE    0x31  // 获取算法类型  
#define AT6010_CMD_GET_DET_INFO     0x30  // 获取雷达感应信息
#define AT6010_CMD_GET_BOUNDARY     0x32  // 获取算法配置边界值
#define AT6010_CMD_GET_ALGO_CFG     0x33  // 获取算法感应配置
#define AT6010_CMD_SET_DET_ON       0xD1  // 打开雷达感应
#define AT6010_CMD_GET_DET_STATUS   0xD0  // 获取雷达感应状态
#define AT6010_CMD_SET_MOT_DIST_MAX 0xD2  // 设置运动最远距离
#define AT6010_CMD_SET_MOT_DIST_MIN 0x34  // 设置运动最近距离
#define AT6010_CMD_SET_MOT_SENS     0x35  // 设置运动检测灵敏度

// ============ 帧构建/解析 ============

// 构建命令帧 (带2字节校验和)
// 返回帧字节数, 0=失败
static size_t buildCommand(uint8_t cmd_packed, const uint8_t *params,
                            uint8_t param_len, uint8_t *buf, size_t buf_size) {
    // 帧格式: HEAD(1) + CMD(1) + LEN(1) + PARAMS(N) + CHECK(2)
    size_t total = 3 + param_len + 2;  // 1+1+1+N+2
    if (!buf || buf_size < total) return 0;
    
    size_t pos = 0;
    buf[pos++] = 0x58;          // HEAD
    buf[pos++] = cmd_packed;    // CMD (packed)
    buf[pos++] = param_len;     // LEN
    for (int i = 0; i < param_len; i++) {
        buf[pos++] = params[i];
    }
    
    // 2字节校验和 (LE)
    uint16_t sum = 0;
    for (size_t i = 0; i < pos; i++) sum += buf[i];
    buf[pos++] = sum & 0xFF;        // low byte
    buf[pos++] = (sum >> 8) & 0xFF; // high byte
    
    return pos;
}

// 快速发送命令的便捷函数 (需要传入 HardwareSerial)
static void sendAT6010Cmd(HardwareSerial *ser, uint8_t cmd_packed,
                           const uint8_t *params, uint8_t param_len) {
    uint8_t buf[32];
    size_t len = buildCommand(cmd_packed, params, param_len, buf, sizeof(buf));
    if (len > 0) ser->write(buf, len);
}

#endif
