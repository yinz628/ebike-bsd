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
#define I2S_SAMPLE_RATE 16000

#define ES8311_I2C_ADDR  0x18
#define PA_ENABLE_PIN    13    // NS4150B 功放使能 (高=输出)

class AlertSound {
private:
    uint8_t lastMode = 255;
    unsigned long phaseStart;
    unsigned long lastToggle;
    bool toneOn = false;
    bool _ok = false;
    bool _pa_enabled = false;    // 功放使能状态跟踪 (按需开关省电)

    static const int BUF_SAMPLES = 800;   // 100ms @ 8kHz
    int16_t bufSilence[BUF_SAMPLES];
    int16_t bufTone[BUF_SAMPLES];

    void playBuf(const int16_t *buf, size_t n) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, n * 2, &written, portMAX_DELAY);
    }

    void writeES8311Reg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(ES8311_I2C_ADDR);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }

public:
    void init() {
        // 1. 功放使能引脚初始化 — 默认关闭省电, 播放时才使能
        pinMode(PA_ENABLE_PIN, OUTPUT);
        digitalWrite(PA_ENABLE_PIN, LOW);
        Serial.println("[SND] 功放 NS4150B 初始化 (默认关闭)");

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
        pins.mck_io_num       = I2S_MCLK;
        i2s_set_pin(I2S_NUM_0, &pins);

        // 3. 预生成波形 (2kHz 方波 + 静音)
        for (int i = 0; i < BUF_SAMPLES; i++) {
            bufTone[i]    = ((i % 8) < 4) ? 10000 : -10000;
            bufSilence[i] = 0;
        }

        // 4. ES8311 寄存器配置
        initES8311Basic();

        _ok = true;
        Serial.println("[SND] I2S + ES8311 就绪 (功放默认关闭)");
    }

    // 主循环调用: 按 mode 播放
    // 功放按需使能: 只在本轮确实要输出方波时才开 PA + 推 I2S,
    //   静音/静默期直接关 PA + return, 不推 DMA (省电 + 减少 loop 阻塞)
    void update(uint8_t mode) {
        if (!_ok) return;

        unsigned long now = millis();

        // 模式变化时重置相位计时
        if (mode != lastMode) {
            lastMode = mode;
            phaseStart = now;
            lastToggle = now;
            toneOn = (mode != 0);
        }

        // 判断本轮是否需要出声 (是否要推方波)
        bool wantTone = false;
        switch (mode) {
            case 0:                          // 静音: 不出声
                wantTone = false;
                break;
            case 1:                          // BSD 单次短鸣: 仅前 100ms 出声
                wantTone = (now - phaseStart < 100);
                break;
            case 2:                          // RCW 4Hz: 125ms on / 125ms off
                if (now - lastToggle > 125) {
                    toneOn = !toneOn;
                    lastToggle = now;
                }
                wantTone = toneOn;
                break;
            case 3:                          // 转向辅助: 持续出声
                wantTone = true;
                break;
        }

        // 按需控制功放: 要出声时开, 不出声时关
        if (wantTone && !_pa_enabled) {
            digitalWrite(PA_ENABLE_PIN, HIGH);
            _pa_enabled = true;
        } else if (!wantTone && _pa_enabled) {
            digitalWrite(PA_ENABLE_PIN, LOW);
            _pa_enabled = false;
        }

        // 只在需要出声时推 I2S DMA, 否则直接返回 (不推静音缓冲)
        if (!wantTone) return;

        // 所有报警模式都播放同一段 2kHz 方波, 区别仅在 wantTone 的时序逻辑
        playBuf(bufTone, BUF_SAMPLES);
    }

private:
    void initES8311Basic() {
        // 1) 复位序列
        writeES8311Reg(0x00, 0x1F);
        delay(20);
        writeES8311Reg(0x00, 0x00);
        writeES8311Reg(0x00, 0x80);   // 上电

        // 2) 时钟配置
        writeES8311Reg(0x01, 0x3F);
        writeES8311Reg(0x02, 0x48);
        writeES8311Reg(0x03, 0x10);
        writeES8311Reg(0x04, 0x10);
        writeES8311Reg(0x05, 0x00);
        writeES8311Reg(0x06, 0xC3);
        writeES8311Reg(0x07, 0xC0);
        writeES8311Reg(0x08, 0xFF);

        // 3) 数据格式
        writeES8311Reg(0x09, 0x0C);
        writeES8311Reg(0x0A, 0x0C);

        // 4) 模拟上电
        writeES8311Reg(0x0D, 0x01);
        writeES8311Reg(0x0E, 0x02);
        writeES8311Reg(0x12, 0x00);
        writeES8311Reg(0x13, 0x10);
        writeES8311Reg(0x1C, 0x6A);
        writeES8311Reg(0x37, 0x08);

        // 5) 音量
        writeES8311Reg(0x32, 0xB2);   // ~70%

        Serial.println("[SND] ES8311 寄存器已配置 (16kHz/16bit/I2S slave)");
    }
};
