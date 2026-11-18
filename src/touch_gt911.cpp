// GT911 capacitive touch over I2C (Arduino Wire). Used by the ESP32-P4-WIFI6-Touch-LCD-7B.
// Device-only; same interface as the other touch drivers (touch.h).
//
// As in Waveshare's Arduino example for the board, the GT911's INT and RST lines are left
// alone and the chip is polled. Its I2C address depends on how INT sat when it left reset
// (0x5D or 0x14), so both are probed.
//
// The GT911 produces a report roughly every 10 ms while a finger is down and exactly one
// "0 points" report when it lifts. Register 0x814E bit 7 says a new report is waiting; when
// it is clear nothing changed since the last read, which is NOT a lift. So the last state
// is kept and repeated, and only a "0 points" report (or reports stopping altogether for
// GT_SILENT_RELEASE_MS while down) ends a press.
#include "touch.h"
#include "config.h"

#if TOUCH_DRIVER_GT911
#include <Arduino.h>
#include <Wire.h>

#define GT_REG_PRODUCT_ID   0x8140      // 4 ASCII bytes, "911"
#define GT_REG_STATUS       0x814E      // bit7 = report ready, low nibble = touch points
#define GT_REG_POINT1       0x814F      // track id, x lo, x hi, y lo, y hi, size lo, size hi
#define GT_STATUS_READY     0x80
#define GT_MAX_POINTS       5
#define GT_REPROBE_MS       1000        // not found yet: look again this often
#define GT_SILENT_RELEASE_MS 150        // down, but no report for this long: treat as lifted
#define GT_FAIL_LOG_MS      5000        // TOUCH_LOG_EDGES: report failing reads at most this often

#ifndef TOUCH_LOG_EDGES
#define TOUCH_LOG_EDGES     0           // 1 = log presses, lifts and failing reads (board header)
#endif

static uint8_t  s_addr = 0;             // 0 = not found (yet)
static uint32_t s_lastProbeMs = 0;
static bool     s_down = false;
static uint16_t s_x = 0, s_y = 0;
static uint32_t s_lastReportMs = 0;
#if TOUCH_LOG_EDGES
static bool     s_loggedDown = false;   // the press start was logged
static uint16_t s_rawX = 0, s_rawY = 0; // controller coordinates before clamp/mirror
static uint32_t s_failCount = 0, s_failLogMs = 0;
#endif

static bool gt_read(uint8_t addr, uint16_t reg, uint8_t *data, uint8_t len) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;   // repeated START
    if (Wire.requestFrom(addr, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
    return true;
}

static bool gt_write(uint8_t addr, uint16_t reg, uint8_t value) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool gt_probe() {
    static const uint8_t addrs[2] = { I2C_ADDR_TOUCH, I2C_ADDR_TOUCH_ALT };
    for (uint8_t a : addrs) {
        uint8_t id[4] = {0, 0, 0, 0};
        if (!gt_read(a, GT_REG_PRODUCT_ID, id, 4)) continue;
        s_addr = a;
        Serial.printf("[touch] GT911 at 0x%02X, product id \"%c%c%c%c\"\n", a,
                      id[0] ? id[0] : ' ', id[1] ? id[1] : ' ', id[2] ? id[2] : ' ', id[3] ? id[3] : ' ');
        return true;
    }
    return false;
}

bool touch_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    s_lastProbeMs = millis();
    if (!gt_probe()) {
        // Not fatal: the controller may still be coming out of the panel's power-up.
        Serial.println("[touch] GT911 not responding at 0x5D or 0x14 yet (will keep probing)");
    }
    return true;
}

TouchRead touch_read(uint16_t *ox, uint16_t *oy) {
    const uint32_t now = millis();
    if (!s_addr) {
        if (now - s_lastProbeMs < GT_REPROBE_MS) return TOUCH_NODATA;
        s_lastProbeMs = now;
        if (!gt_probe()) return TOUCH_NODATA;
    }

    uint8_t status = 0;
    if (!gt_read(s_addr, GT_REG_STATUS, &status, 1)) {
#if TOUCH_LOG_EDGES
        ++s_failCount;
        if (now - s_failLogMs >= GT_FAIL_LOG_MS) {
            s_failLogMs = now;
            Serial.printf("[touch] GT911 status read failing at 0x%02X (%lu failures so far)\n",
                          s_addr, (unsigned long)s_failCount);
        }
#endif
        return TOUCH_NODATA;
    }

    if (status & GT_STATUS_READY) {
        const uint8_t points = status & 0x0F;
        bool ok = true;
        if (points == 0 || points > GT_MAX_POINTS) {
            s_down = false;                               // the lift report (or garbage: release)
        } else {
            uint8_t d[7];
            ok = gt_read(s_addr, GT_REG_POINT1, d, sizeof(d));
            if (ok) {
                uint16_t x = (uint16_t)(d[1] | (d[2] << 8));
                uint16_t y = (uint16_t)(d[3] | (d[4] << 8));
#if TOUCH_LOG_EDGES
                s_rawX = x; s_rawY = y;
#endif
                if (x > PANEL_W - 1) x = PANEL_W - 1;
                if (y > PANEL_H - 1) y = PANEL_H - 1;
                if (TP_MIRROR_X) x = (uint16_t)((PANEL_W - 1) - x);
                if (TP_MIRROR_Y) y = (uint16_t)((PANEL_H - 1) - y);
                s_x = x; s_y = y; s_down = true;
            }
        }
        gt_write(s_addr, GT_REG_STATUS, 0);               // acknowledge: lets the next report in
        if (!ok) return TOUCH_NODATA;
        s_lastReportMs = now;
    } else if (s_down && now - s_lastReportMs > GT_SILENT_RELEASE_MS) {
        s_down = false;                                   // reports stopped without a lift
    }

#if TOUCH_LOG_EDGES
    if (s_down && !s_loggedDown) {
        Serial.printf("[touch] down raw %u,%u -> panel %u,%u\n", s_rawX, s_rawY, s_x, s_y);
        s_loggedDown = true;
    } else if (!s_down && s_loggedDown) {
        Serial.printf("[touch] up   at panel %u,%u\n", s_x, s_y);
        s_loggedDown = false;
    }
#endif

    if (!s_down) return TOUCH_UP;
    *ox = s_x;
    *oy = s_y;
    return TOUCH_DOWN;
}

#endif  // TOUCH_DRIVER_GT911
