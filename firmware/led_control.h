// ============================================================
//  led_control.h - LED PWM 控制 (ESP32 Arduino Core 3.x 兼容)
//  封装4路MOSFET驱动的GPIO输出
// ============================================================
#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>

#define PWM_FREQ     5000    // 5kHz, 足以驱动LED无闪烁
#define PWM_RES      8       // 8bit 分辨率 (0-255)
#define PWM_DUTY_ON  255     // 100% 亮度
#define PWM_DUTY_OFF 0

class LEDController {
private:
    uint8_t _pins[4];
    bool    _initialized[4];
    bool    _pwm_attached[4];   // 该通道是否已 attach PWM
    int     _count;

public:
    LEDController() : _count(0) {
        for (int i = 0; i < 4; i++) {
            _pins[i] = 0;
            _initialized[i] = false;
            _pwm_attached[i] = false;
        }
    }

    // 初始化4个LED通道 (ESP32 Core 3.x: ledcAttach 自动分配通道)
    void begin(uint8_t pin_l, uint8_t pin_r, uint8_t = 0, uint8_t = 0) {
        addChannel(pin_l);
        addChannel(pin_r);
        allOff();
    }

    void addChannel(uint8_t gpio) {
        if (_count >= 4) return;
        pinMode(gpio, OUTPUT);
        digitalWrite(gpio, LOW);
        _pins[_count] = gpio;
        _initialized[_count] = true;
        _count++;
    }

    // 设置某个GPIO的LED
    void set(uint8_t gpio, bool on) {
        pinMode(gpio, OUTPUT);  // 确保引脚模式
        digitalWrite(gpio, on ? HIGH : LOW);
    }

    // PWM调光 (0-255). 首次调用时按需 attach, 兼容 ESP32 Core 2.x/3.x
    void setBrightness(uint8_t gpio, uint8_t duty) {
        for (int i = 0; i < _count; i++) {
            if (_pins[i] == gpio) {
                if (!_pwm_attached[i]) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
                    // Core 3.x: ledcAttach(pin, freq, res)
                    ledcAttach(gpio, PWM_FREQ, PWM_RES);
#else
                    // Core 2.x: channel-based API
                    ledcSetup(i, PWM_FREQ, PWM_RES);
                    ledcAttachPin(gpio, i);
#endif
                    _pwm_attached[i] = true;
                }
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
                ledcWrite(gpio, duty);
#else
                ledcWrite(i, duty);
#endif
                return;
            }
        }
    }

    void allOff() {
        for (int i = 0; i < _count; i++) {
            if (_initialized[i]) digitalWrite(_pins[i], LOW);
        }
    }

    void allOn() {
        for (int i = 0; i < _count; i++) {
            if (_initialized[i]) digitalWrite(_pins[i], HIGH);
        }
    }

    int count() { return _count; }
};

#endif // LED_CONTROL_H
