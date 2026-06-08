// ES8311 codec "ping" generator. See audio.h for the core/bus discipline.
//
// The ES8311 register init below is the canonical DAC-playback sequence (MCLK from
// the I2S MCLK pin). If the speaker stays silent on hardware, cross-check the values
// against the Waveshare 08_ES8311 Arduino demo — only this table is board-specific;
// the triggers, volume and task wiring are independent of it.
#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <math.h>

#define ES8311_ADDR   0x18
#define SR            16000          // playback sample rate (a beep; pitch-tolerant)

static I2SClass s_i2s;
static bool     s_ok = false;
static volatile int  s_vol = 60;     // 0..100
static volatile bool s_muted = false;
static volatile int  s_cue = -1;
static SemaphoreHandle_t s_sem = nullptr;

static void es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0;
    return Wire.read();
}

// Standard ES8311 DAC-playback init (I2S slave, MCLK present).
static void es8311_init() {
    static const uint8_t seq[][2] = {
        {0x00,0x1F},{0x01,0x30},{0x02,0x10},{0x03,0x10},{0x16,0x24},{0x04,0x10},
        {0x05,0x00},{0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7C},{0x00,0x80},
        {0x0D,0x01},{0x0E,0x02},{0x12,0x00},{0x13,0x10},{0x32,0xBF},{0x37,0x48},
        {0x44,0x08},{0x00,0x80},
    };
    for (auto &r : seq) { es_write(r[0], r[1]); delay(1); }
}

// Synthesize one beep (freq Hz, ms) with a short fade in/out, into a stereo buffer.
static size_t gen_beep(int16_t *buf, size_t cap, float freq, int ms, float amp) {
    const size_t n = (size_t)((long)SR * ms / 1000);
    const size_t fade = SR / 200;                 // ~5 ms ramps (anti-click)
    size_t i = 0;
    for (; i < n && (i * 2 + 1) < cap; ++i) {
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > n - fade)   env = (float)(n - i) / fade;
        const int16_t s = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq * i / SR));
        buf[i * 2] = s; buf[i * 2 + 1] = s;       // L = R
    }
    return i * 2;                                  // samples written (stereo interleaved)
}

static void play_cue(int cue) {
    if (!s_ok || s_muted || s_vol <= 0) return;
    static int16_t buf[SR / 2 * 2];                // up to 500 ms stereo
    const float amp = (s_vol / 100.0f) * 17000.0f;
    digitalWrite(PIN_AUDIO_PA, HIGH);              // enable speaker amp
    delay(2);
    if (cue == AUDIO_ALERT) {
        for (int k = 0; k < 2; ++k) {
            size_t ns = gen_beep(buf, sizeof(buf) / 2, 1320.0f, 80, amp);
            s_i2s.write((uint8_t *)buf, ns * 2);
            delay(40);
        }
    } else {
        size_t ns = gen_beep(buf, sizeof(buf) / 2, 880.0f, 110, amp * 0.8f);
        s_i2s.write((uint8_t *)buf, ns * 2);
    }
    delay(6);
    digitalWrite(PIN_AUDIO_PA, LOW);               // mute amp between pings (saves power, kills hiss)
}

static void audio_task(void *) {
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) == pdTRUE) {
            const int cue = s_cue;
            play_cue(cue);
        }
    }
}

bool audio_begin() {
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, LOW);

    const uint8_t id1 = es_read(0xFD), id2 = es_read(0xFE);   // expect 0x83, 0x11
    if (id1 != 0x83) {
        Serial.printf("[audio] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
        s_ok = false;
        return false;
    }
    es8311_init();

    s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT, -1, PIN_I2S_MCLK);
    if (!s_i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        Serial.println("[audio] I2S begin failed");
        s_ok = false;
        return false;
    }
    s_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 1, nullptr, 0);  // I2S only -> core 0
    s_ok = true;
    Serial.println("[audio] ES8311 ready");
    return true;
}

bool audio_present() { return s_ok; }
void audio_set_volume(int pct) { s_vol = constrain(pct, 0, 100); }
void audio_set_muted(bool m) { s_muted = m; }

void audio_play(AudioCue cue) {
    if (!s_ok || s_muted) return;
    s_cue = (int)cue;
    if (s_sem) xSemaphoreGive(s_sem);
}
