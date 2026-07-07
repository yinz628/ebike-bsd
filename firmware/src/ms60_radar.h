// ============================================================
//  ms60_radar.h - MS60-3015 60GHz 雷达驱动
//  功能: UART初始化、配置BSD模式、帧同步解析
// ============================================================
#ifndef MS60_RADAR_H
#define MS60_RADAR_H

#include <Arduino.h>
#include "bsd_protocol.h"
#include "config_store.h"

// 接收缓冲区大小
#define RADAR_RX_BUF_SIZE       256
// 帧同步超时 (ms) - 在收到0x58后等待完整帧
#define FRAME_TIMEOUT_MS        20
// 最大帧长度 (含校验)
#define MAX_FRAME_LEN           64

class MS60Radar {
private:
    int  _uart_num;       // UART 编号 (0/1/2)
    int  _rx_pin;
    int  _tx_pin;
    int  _baud;
    
    HardwareSerial *_serial;  // 指向实际 Serial 对象
    
    // 接收状态机
    uint8_t _rx_buf[RADAR_RX_BUF_SIZE];
    size_t  _rx_len;
    bool    _frame_started;
    unsigned long _frame_start_time;
    
    // 最新解析结果
    BSDFrame _frame;
    
    // 诊断计数
    unsigned long _total_bytes_read;
    uint8_t _raw_log[48];     // 前48字节 raw hex dump
    int _raw_log_len;

public:
    unsigned long getTotalBytes() { return _total_bytes_read; }
    const uint8_t* getRawLog() { return _raw_log; }
    int getRawLogLen() { return _raw_log_len; }

public:
    MS60Radar(int uart_num, int rx_pin, int tx_pin, int baud)
        : _uart_num(uart_num), _rx_pin(rx_pin), _tx_pin(tx_pin),
          _baud(baud), _serial(nullptr), _rx_len(0),
          _frame_started(false), _frame_start_time(0),
          _total_bytes_read(0), _raw_log_len(0) {
        
        memset(_rx_buf, 0, sizeof(_rx_buf));
        memset(&_frame, 0, sizeof(_frame));
    }

    // ============ 初始化 ============
    bool begin() {
        switch (_uart_num) {
            case 1:
                _serial = &Serial1;
                break;
            case 2:
                _serial = &Serial2;
                break;
            default:
                return false;
        }
        
        // 初始化 UART
        _serial->begin(_baud, SERIAL_8N1, _rx_pin, _tx_pin);
        
        // 清空缓冲区
        flush();
        
        // 延时等待模块启动
        delay(100);
        
        return true;
    }
    
    // ============ 设置BSD模式 (正确AT6010格式) ============
    void setBSDMode() {
        if (!_serial) return;
        
        uint8_t sens = constrain(config.radar.sensitivity, 0, 10);
        uint16_t range_cm = (uint16_t)config.radar.det_range * 100;  // m→cm
        
        auto send = [this](uint8_t cmd, const uint8_t* params, uint8_t plen) {
            uint8_t buf[16];
            buf[0] = 0x58; buf[1] = cmd; buf[2] = plen;
            for(int i=0;i<plen;i++) buf[3+i]=params[i];
            uint8_t sum=0;
            for(int i=0;i<3+plen;i++) sum+=buf[i];
            buf[3+plen]=sum;  // 1字节校验和 (HEAD+CMD+LEN+PARAMS)
            _serial->write(buf, 4+plen);
        };
        
        // 1. 打开雷达感应 (0xD1, §3.2.1)
        uint8_t p1[] = {0x01};
        send(0xD1, p1, 1);
        delay(100);
        
        // 2. 设置运动检测灵敏度 (0x35, §3.2.11, 0~10越小越灵敏)
        uint8_t p2[] = {sens};
        send(0x35, p2, 1);
        delay(100);
        
        // 3. 设置运动检测最远距离 (0xD2, §3.2.9, u16 LE 单位cm)
        uint8_t p3[] = {(uint8_t)(range_cm & 0xFF), (uint8_t)(range_cm >> 8)};
        send(0xD2, p3, 2);
        delay(100);
        
        // 4. 保存设置到Flash (0x05, §3.1.5, 分段yield防看门狗)
        send(0x05, {}, 0);
        for (int i = 0; i < 12; i++) { delay(100); yield(); }
        
        flush();
    }
    
    // ============ 读取并解析帧 ============
    // 每次loop调用, 自动从UART读取字节并进行帧同步
    void readFrame() {
        if (!_serial) return;
        
        // 读取所有可用字节
        while (_serial->available()) {
            uint8_t byte = _serial->read();
            _total_bytes_read++;  // 诊断计数
            // 抓前48字节 raw hex
            if (_raw_log_len < 48) {
                _raw_log[_raw_log_len++] = byte;
            }
            
            if (!_frame_started) {
                // 等待帧头
                if (byte == BSD_FRAME_HEADER) {
                    _frame_started = true;
                    _frame_start_time = millis();
                    _rx_len = 0;
                    _rx_buf[_rx_len++] = byte;
                }
            } else {
                // 帧同步进行中
                if (_rx_len < RADAR_RX_BUF_SIZE) {
                    _rx_buf[_rx_len++] = byte;
                } else {
                    // 缓冲区溢出，放弃该帧
                    _frame_started = false;
                    _rx_len = 0;
                    continue;
                }
                
                // 检查是否已收到完整帧 (基于LEN字节)
                if (_rx_len >= 3) {
                    uint8_t len = _rx_buf[1];           // byte1 = LEN (PAYLOAD字节数)
                    size_t expected = 2 + len + 1;      // HEAD(1)+LEN(1)+PAYLOAD(LEN)+CHECK(1)
                    
                    if (_rx_len >= expected) {
                        // 尝试解析
                        BSDFrame new_frame;
                        if (BSDProtocol::parseFrame(_rx_buf, _rx_len, &new_frame)) {
                            if (BSDProtocol::isBSDFrame(new_frame.header,
                                                         new_frame.type)) {
                                _frame = new_frame;  // 更新最新帧
                            }
                        }
                        // 复位状态机，等待下一帧
                        _frame_started = false;
                        _rx_len = 0;
                    }
                }
                
                // 超时保护
                if (_frame_started && (millis() - _frame_start_time) > FRAME_TIMEOUT_MS) {
                    _frame_started = false;
                    _rx_len = 0;
                }
            }
        }
    }
    
    // ============ 获取最新帧 ============
    BSDFrame* getFrame() {
        return &_frame;
    }
    
    // ============ 检查帧是否过时 (雷达静默时清零目标) ============
    void checkStale(unsigned long timeout_ms = 500) {
        if (_frame.timestamp > 0 && (millis() - _frame.timestamp) > timeout_ms) {
            _frame.obj_num = 0;
            memset(_frame.objects, 0, sizeof(_frame.objects));
        }
    }
    
    // ============ 发送命令 ============
    void sendCmd(const uint8_t *cmd, size_t len) {
        if (!_serial || !cmd || len == 0) return;
        _serial->write(cmd, len);
    }
    
    // ============ 清空接收缓冲区 ============
    void flush() {
        if (!_serial) return;
        while (_serial->available()) {
            _serial->read();
        }
        _rx_len = 0;
        _frame_started = false;
        _frame_start_time = 0;
        memset(&_frame, 0, sizeof(_frame));
    }
    
    // ============ 检测状态 ============
    bool isConnected() {
        return (_serial != nullptr) && (_frame.obj_num > 0 || _frame.timestamp > 0);
    }
};

#endif // MS60_RADAR_H
