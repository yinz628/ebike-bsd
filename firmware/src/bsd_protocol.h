// ============================================================
//  bsd_protocol.h — 基于 AT6010 HCI Protocol v1.4
//  主动上报格式: 5A + LEN + PAYLOAD(LEN bytes) + CHECK(1B)
//  命令格式:     58 + CMD_PACKED + CMD_LEN + PARAMS + CHECK(2B)
//  BSD TARGET: TYPE=0x07 (vendor-specific BSD report)
// ============================================================
#ifndef BSD_PROTOCOL_H
#define BSD_PROTOCOL_H

#include <Arduino.h>
#include "config_store.h"

// ============ 帧常量 ============
#define HEAD_CMD      0x58
#define HEAD_RESP     0x59
#define HEAD_REPORT   0x5A

// ============ 主动上报 TYPE ============
#define REPORT_TYPE_COMPLETE    0x00  // 完整检测信息 (fmcw_det_info_t)
#define REPORT_TYPE_BSD_VENDOR  0x07  // BSD 主动上报 (vendor specific)

// ============ BSD 目标对象 ============
typedef struct {
    int8_t  angle;       // 角度, 单位: ° (signed)
    uint8_t status;      // 目标状态: 0x03=运动, 0x04=微动
    int8_t  range;       // 距离, 单位: m (暂未从TYPE=7获取)
    int8_t  velocity;    // 速度, 单位: m/s (暂未从TYPE=7获取)
    uint8_t objId;       // 目标 ID
} __attribute__((packed)) BSDObj;

// ============ 上报帧 ============
#define BSD_MAX_OBJECTS  4

typedef struct {
    uint8_t  header;       // 0x5A
    uint8_t  len;          // PAYLOAD 字节数
    uint8_t  type;         // 上报类型
    uint8_t  obj_num;      // 目标数量
    BSDObj   objects[BSD_MAX_OBJECTS];
    uint8_t  checksum;
    bool     valid;
    uint32_t timestamp;
} BSDFrame;

// ============ 角度分区 ============


// ============ 协议解析器 ============
class BSDProtocol {
public:
    // 解析主动上报帧
    // 格式: 5A + LEN + TYPE + payload(LEN-1) + CHECK
    static bool parseFrame(const uint8_t *rawData, size_t len, BSDFrame *frame) {
        if (!rawData || !frame || len < 5) return false;
        
        memset(frame->objects, 0, sizeof(frame->objects));
        frame->valid = false;
        frame->timestamp = millis();
        
        size_t pos = 0;
        frame->header = rawData[pos++];
        if (frame->header != HEAD_REPORT) return false;
        
        frame->len = rawData[pos++];
        if (frame->len < 2) return false;  // need at least TYPE + something
        
        // Check we have full frame
        size_t expected_total = 2 + frame->len + 1;  // HEAD + LEN + PAYLOAD + CHECK
        if (len < expected_total) return false;
        
        // PAYLOAD starts here
        frame->type = rawData[pos++];
        
        // Parse based on TYPE
        if (frame->type == REPORT_TYPE_BSD_VENDOR) {
            // TYPE=0x07: BSD上报 (§3.4.8官方格式, 校验和已验证✅)
            // 格式: obj_num(2B LE) + reserved(2B) + [target * 4B] × N
            // 每目标4B: range(s8,m) + angle(s8,°) + velo(s8,m/s) + objId(s8)
            if (frame->len < 6) return false;
            
            frame->obj_num = rawData[pos] | (rawData[pos+1] << 8);
            pos += 2;
            if (frame->obj_num > BSD_MAX_OBJECTS) frame->obj_num = BSD_MAX_OBJECTS;
            pos += 2;  // skip reserved (2B)
            
            for (int i = 0; i < frame->obj_num; i++) {
                if (pos + 4 > 2 + frame->len) break;
                frame->objects[i].range    = (int8_t)rawData[pos++];  // 距离, m
                frame->objects[i].angle    = -(int8_t)rawData[pos++]; // 角度, ° (180°安装取反)
                frame->objects[i].velocity = -(int8_t)rawData[pos++]; // 速度, m/s (雷达正负反)
                frame->objects[i].objId    = rawData[pos++];           // 目标ID
                frame->objects[i].status   = 0x03;                    // 默认运动
            }
        }
        else if (frame->type == REPORT_TYPE_COMPLETE) {
            // TYPE=0x00: Complete detection info (20 bytes = 1+1+2+2+2+6+1+1+4)
            // Simplified: just extract angle and status
            if (frame->len >= 7) {
                frame->obj_num = 1;
                pos++;  // is_detected
                pos++;  // det_result
                // range_val (2 bytes LE)
                uint16_t range_mm = rawData[pos] | (rawData[pos+1] << 8);
                frame->objects[0].range = range_mm / 1000;  // mm → m
                pos += 2;
                // angle_val (2 bytes LE, signed)
                int16_t angle_deg = rawData[pos] | (rawData[pos+1] << 8);
                frame->objects[0].angle = angle_deg;
                pos += 2;
                frame->objects[0].status = 0x03;  // assume moving
                frame->objects[0].velocity = 0;
                frame->objects[0].objId = 0;
            }
        }
        
        // Verify checksum: sum(HEAD+LEN+PAYLOAD)
        frame->checksum = rawData[1 + frame->len + 1];  // position = 1 + len + 1
        uint8_t calc = 0;
        for (size_t i = 0; i < 2 + frame->len; i++) {  // HEAD + LEN + PAYLOAD
            calc += rawData[i];
        }
        frame->valid = (calc == frame->checksum);
        return true;
    }
    
    // 判断是不是BSD主动上报帧
    static bool isBSDFrame(uint8_t header, uint8_t type) {
        return (header == HEAD_REPORT && type == REPORT_TYPE_BSD_VENDOR);
    }
    
    // --- 角度判断工具 ---
    static bool isInLeftBlindSpot(const BSDObj *obj) {
        return (obj->status > 0 && ANGLE_IS_LEFT(obj->angle));
    }
    static bool isInRightBlindSpot(const BSDObj *obj) {
        return (obj->status > 0 && ANGLE_IS_RIGHT(obj->angle));
    }
    static bool isInLeftTurnZone(const BSDObj *obj) {
        return (obj->status > 0 && TURN_ANGLE_IS_LEFT(obj->angle));
    }
    static bool isInRightTurnZone(const BSDObj *obj) {
        return (obj->status > 0 && TURN_ANGLE_IS_RIGHT(obj->angle));
    }
    static bool isDangerous(const BSDObj *obj) {
        return (obj->velocity >= 2);
    }
};

// ============ 兼容常量 (BSD_FRAME_HEADER 被 ms60_radar.h 使用) ============
#define BSD_FRAME_HEADER       HEAD_REPORT

#endif
