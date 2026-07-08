// ============================================================
//  alert_sound.h - 报警音同步 (P4)
//  按 buzzer_mode 在 C3 扬声器播放, 与车尾蜂鸣器同步:
//    0=静音  1=BSD单鸣  2=RCW 4Hz  3=转向长鸣
//
//  引脚来源: 立创官方 IDF 例程 06-i2s_es8311 (C3 分支)
//    I2S: BCK=GPIO8, WS(LRCK)=GPIO12, DOUT=GPIO11, DIN=GPIO7, MCLK=GPIO10
//    ES8311 I2C: 与触摸共用 SDA=GPIO0/SCL=GPIO1, 地址 0x18
//    功放 NS4150B 使能: GPIO13 (高电平允许输出, 不拉高喇叭不响)
// ============================================================
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

#define I2S_BCK   8
#define I2S_WS    12    // LRCK
#define I2S_DOUT  11
#define I2S_DIN   7
#define I2S_MCLK  10
#define I2S_SAMPLE_RATE 8000

#define ES8311_I2C_ADDR  0x18
#define PA_ENABLE_PIN    13    // NS4150B 功放使能

class AlertSound {
private:
    uint8_t lastMode = 255;     // 上次模式 (检测变化)
    unsigned long phaseStart;   // 当前模式开始时间
    unsigned long lastToggle;   // 4Hz 闪烁计时
    bool toneOn = false;
    bool _ok = false;           // 初始化是否成功

    // 简单方波样本缓冲
    static const int BUF_SAMPLES = 400;   // 50ms @ 8kHz
    int16_t bufSilence[BUF_SAMPLES];
    int16_t bufTone[BUF_SAMPLES];

    void playBuf(const int16_t *buf, size_t n) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, n * 2, &written, 0);   // 非阻塞, 0 = 不等待
    }

    // ES8311 寄存器写入辅助
    void writeES8311Reg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(ES8311_I2C_ADDR);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }

public:
    void init() {
        // 1. 先打开功放 (NS4150B, GPIO13 高电平使能)
        pinMode(PA_ENABLE_PIN, OUTPUT);
        digitalWrite(PA_ENABLE_PIN, HIGH);
        Serial.println("[SND] 功放 NS4150B 已使能 (GPIO13=HIGH)");

        // 2. 初始化 I2S
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
        pins.bck_io_num       = I2S_BCK;
        pins.ws_io_num        = I2S_WS;
        pins.data_out_num     = I2S_DOUT;
        pins.data_in_num      = I2S_DIN;
        pins.mck_io_num       = I2S_MCLK;     // ⚠ 必须配 MCLK, ES8311 需要主时钟
        i2s_set_pin(I2S_NUM_0, &pins);

        // 3. 预生成波形 (1kHz 方波 + 静音)
        for (int i = 0; i < BUF_SAMPLES; i++) {
            bufTone[i]    = (i < BUF_SAMPLES / 2) ? 8000 : -8000;
            bufSilence[i] = 0;
        }

        // 4. ES8311 寄存器初始化 (I2C 配成 I2S → DAC 播放模式)
        //    寄存器序列参考 ES8311 datasheet + 立创实战派 06-i2s_es8311 例程
        Wire.begin(0, 1);   // 共用板载 I2C 总线 SDA=0/SCL=1
        initES8311Basic();

        _ok = true;
        Serial.println("[SND] I2S + ES8311 就绪");
    }

    // 主循环调用: 按 mode 播放
    void update(uint8_t mode) {
        if (!_ok) return;
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
    // 参考 ES8311 datasheet + 乐鑫 es8311 驱动 (components/es8311)
    void initES8311Basic() {
        // 复位
        writeES8311Reg(0x01, 0x80);
        delay(50);
        writeES8311Reg(0x01, 0x3F);   // 上电, 不复位

        // 时钟配置: 用 MCLK, BCK 自 MCLK 分频
        writeES8311Reg(0x02, 0x00);   // 0x02: 从 MCLK 分频, 内部

        // PLL/DAC 相关 (典型播放配置)
        writeES8311Reg(0x0B, 0x00);
        writeES8311Reg(0x0C, 0x00);
        writeES8311Reg(0x0D, 0x01);   // DAC 使能
        writeES8311Reg(0x0E, 0x02);   // 0x0E: ADC/DAC 相关
        writeES8311Reg(0x10, 0x1F);
        writeES8311Reg(0x11, 0x7F);
        writeES8311Reg(0x12, 0x00);   // DAC 音量 0=最大
        writeES8311Reg(0x13, 0x10);
        writeES8311Reg(0x14, 0x1A);   // DAC 输出路径 (LOUT/ROUT)
        writeES8311Reg(0x15, 0x40);
        writeES8311Reg(0x16, 0x00);
        writeES8311Reg(0x1B, 0x0A);
        writeES8311Reg(0x1C, 0x6C);
        writeES8311Reg(0x1D, 0x60);
        writeES8311Reg(0x1E, 0xCB);
        writeES8311Reg(0x1F, 0x40);
        writeES8311Reg(0x20, 0x40);
        writeES8311Reg(0x21, 0x40);
        writeES8311Reg(0x22, 0x00);
        writeES8311Reg(0x23, 0x00);
        writeES8311Reg(0x24, 0x00);
        writeES8311Reg(0x25, 0x00);
        writeES8311Reg(0x26, 0x02);
        writeES8311Reg(0x37, 0x48);   // 系统/模拟配置
        Serial.println("[SND] ES8311 寄存器已配置");
    }
};
