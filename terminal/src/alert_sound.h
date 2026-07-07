// ============================================================
//  alert_sound.h - 报警音同步 (P4)
//  按 buzzer_mode 在 C3 扬声器播放, 与车尾蜂鸣器同步:
//    0=静音  1=BSD单鸣  2=RCW 4Hz  3=转向长鸣
//
//  注: ES8311 完整初始化较复杂 (I2C 配寄存器 + I2S 数据流),
//      本文件先用最简 I2S 方波驱动。若音质/驱动有问题, P4 可:
//      (a) 用 ES8311 官方初始化序列替换 initES8311()
//      (b) 或退而用 GPIO PWM 压电片(若 C3 有外接蜂鸣器)
// ============================================================
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

#define I2S_WS   7    // LRCK (实战派 ES8311 引脚, 实施时按原理图核对)
#define I2S_BCK  10
#define I2S_DOUT 11
#define I2S_SAMPLE_RATE 8000

class AlertSound {
private:
    uint8_t lastMode = 255;     // 上次模式 (检测变化)
    unsigned long phaseStart;   // 当前模式开始时间
    unsigned long lastToggle;   // 4Hz 闪烁计时
    bool toneOn = false;

    // 简单方波样本缓冲
    static const int BUF_SAMPLES = 400;   // 50ms @ 8kHz
    int16_t bufSilence[BUF_SAMPLES];
    int16_t bufTone[BUF_SAMPLES];

    void playBuf(const int16_t *buf, size_t n) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, n * 2, &written, portMAX_DELAY);
    }

public:
    void init() {
        // 初始化 I2S
        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate = I2S_SAMPLE_RATE;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.tx_desc_auto_clear = true;
        cfg.dma_buf_count = 4;
        cfg.dma_buf_len = 256;
        cfg.use_apll = false;

        if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
            Serial.println("[SND] I2S install FAIL");
            return;
        }
        i2s_pin_config_t pins = {};
        pins.bck_io_num   = I2S_BCK;
        pins.ws_io_num    = I2S_WS;
        pins.data_out_num = I2S_DOUT;
        pins.data_in_num  = I2S_PIN_NO_CHANGE;
        i2s_set_pin(I2S_NUM_0, &pins);

        // 预生成波形 (1kHz 方波 + 静音)
        for (int i = 0; i < BUF_SAMPLES; i++) {
            bufTone[i]    = (i < BUF_SAMPLES / 2) ? 8000 : -8000;
            bufSilence[i] = 0;
        }

        // 注: ES8311 的 I2C 初始化 (寄存器配置成播放模式) 需补充
        initES8311Basic();

        Serial.println("[SND] I2S + ES8311 就绪");
    }

    // 主循环调用: 按 mode 播放
    void update(uint8_t mode) {
        if (mode != lastMode) {
            lastMode = mode;
            phaseStart = millis();
            lastToggle = millis();
            toneOn = (mode != 0);
        }

        unsigned long now = millis();

        switch (mode) {
            case 0:  // 静音
                playBuf(bufSilence, BUF_SAMPLES);
                break;
            case 1:  // BSD 单次短鸣 (100ms)
                if (now - phaseStart < 100) {
                    playBuf(bufTone, BUF_SAMPLES);
                } else {
                    playBuf(bufSilence, BUF_SAMPLES);
                }
                break;
            case 2:  // RCW 4Hz 急促 (125ms on / 125ms off)
                if (now - lastToggle > 125) {
                    toneOn = !toneOn;
                    lastToggle = now;
                }
                playBuf(toneOn ? bufTone : bufSilence, BUF_SAMPLES);
                break;
            case 3:  // 转向辅助 持续长鸣
                playBuf(bufTone, BUF_SAMPLES);
                break;
        }
    }

private:
    // ES8311 基础初始化 (I2C 配成播放模式)
    // 注: 完整寄存器序列参考 ES8311 datasheet / 立创实战派示例
    // 这里给出框架, 实施时按实战派 C3 的 ES8311 引脚(I2C 地址 0x18)补全
    void initES8311Basic() {
        Wire.begin(8, 9);   // 共用触摸的 I2C 总线
        // TODO: 写 ES8311 寄存器使其进入 I2S → DAC 播放模式
        // 关键寄存器 (典型值):
        //   0x01 = 0x80  (复位)
        //   0x01 = 0x3F  (上电)
        //   0x02 = 0x00  (时钟源)
        //   0x0D = 0x01  (DAC 使能)
        //   0x12 = 0x00  (DAC 音量, 0=最大)
        //   0x14 = 0x1A  (DAC 输出路径)
        // 此处先留空, P4 验证时补全
        Serial.println("[SND] ES8311 基础配置 (待补全寄存器序列)");
    }
};
