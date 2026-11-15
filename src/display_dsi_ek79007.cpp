// BOARD_P4_LCD7B display bring-up: EK79007 1024x600 IPS over 2-lane MIPI-DSI on the
// ESP32-P4 (Arduino_GFX's Arduino_DSI_Display, as in Waveshare's own Arduino examples for
// the ESP32-P4-WIFI6-Touch-LCD-7B) + LVGL v8. Implements the same display:: API as the
// AMOLED build (display.cpp), so main.cpp / ui / radar_view stay board-agnostic.
//
// The DSI panel scans out of a framebuffer in PSRAM that Arduino_GFX owns; LVGL renders
// partial areas into an internal-RAM strip and flush_cb copies them in.
//
// Differences from the round boards, all deliberate for a first bring-up:
//   - No sweep compositor (sweepAvailable() is false): LVGL draws the sweep itself, as it
//     does in the desktop simulator. The P4 is several times faster than the S3.
//   - Rotation is 0 or 180 only. The UI is laid out for landscape (radar pane + docked
//     list); any other angle falls back to the nearest of the two.
//   - Brightness is a PWM backlight (active low), not a panel command.
//
// NOT YET RUN ON HARDWARE: see the note at the top of boards/board_p4_lcd_7b.h.
#include "config.h"
#if defined(BOARD_P4_LCD7B)

#include "display.h"
#include "radar_view.h"
#include "ui.h"
#include "touch.h"
#include "touch_queue.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

// EK79007 register table, verbatim from Waveshare's displays_config.h for this board:
// 2-lane pad control, the vendor registers, then sleep out (120 ms).
static const lcd_init_cmd_t EK79007_INIT[] = {
    {0xB2, (uint8_t[]){0x10}, 1, 0},
    {0x80, (uint8_t[]){0x8B}, 1, 0},
    {0x81, (uint8_t[]){0x78}, 1, 0},
    {0x82, (uint8_t[]){0x84}, 1, 0},
    {0x83, (uint8_t[]){0x88}, 1, 0},
    {0x84, (uint8_t[]){0xA8}, 1, 0},
    {0x85, (uint8_t[]){0xE3}, 1, 0},
    {0x86, (uint8_t[]){0x88}, 1, 0},
    {0x11, nullptr, 0, 120},
};

static Arduino_ESP32DSIPanel *s_dsi = nullptr;
static Arduino_DSI_Display   *s_gfx = nullptr;

// --- LVGL plumbing -----------------------------------------------------------
#define LVGL_BUF_LINES 60    // partial draw-buffer height (lines); internal RAM if it fits
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_color_t        *s_buf1 = nullptr;

static volatile uint32_t s_frameCount = 0;   // rendered frames (last-flush), for FPS measurement
uint32_t display_frames() { return s_frameCount; }

static uint16_t s_rot = 0;                   // 0 or 180
static bool     s_ready = false;             // LVGL is up
static bool     s_blReady = false;           // backlight PWM attached

// --- backlight ---------------------------------------------------------------
static void backlight_write(uint8_t v) {
    if (!s_blReady) {
        s_blReady = ledcAttach(PIN_LCD_BL, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
#if LCD_BL_ACTIVE_LOW
        if (s_blReady) ledcOutputInvert(PIN_LCD_BL, true);   // full duty = full brightness
#endif
        if (!s_blReady) { Serial.println("[display] backlight PWM attach FAILED"); return; }
    }
    const uint32_t maxDuty = (1U << LCD_BL_PWM_BITS) - 1U;
    ledcWrite(PIN_LCD_BL, (uint32_t)v * maxDuty / 255U);
}

// --- flush ---------------------------------------------------------------------
static void touch_sample(bool force);

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    touch_sample(false);   // keep sampling the finger between the strips of a long frame
    const int16_t w = (int16_t)(area->x2 - area->x1 + 1);
    const int16_t h = (int16_t)(area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
    s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
#else
    s_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
#endif
    if (lv_disp_flush_is_last(drv)) s_frameCount = s_frameCount + 1;
    lv_disp_flush_ready(drv);
}

// --- touch: GT911 -> LVGL pointer, through TouchQueue (see touch_queue.h) -----------
static TouchQueue s_touchQ;
static bool       s_touchOk = false;
static uint32_t   s_touchLastMs = 0;
static bool       s_touchReplaying = false;

static void touch_sample(bool force) {
    if (!s_touchOk) return;
    const uint32_t now = millis();
    if (!force && now - s_touchLastMs < TOUCH_SAMPLE_MS) return;
    s_touchLastMs = now;
    uint16_t x, y;
    switch (touch_read(&x, &y)) {
        case TOUCH_NODATA: s_touchQ.add(TouchQueue::NODATA, 0, 0, now); break;
        case TOUCH_UP:     s_touchQ.add(TouchQueue::UP, 0, 0, now); break;
        case TOUCH_DOWN:
            if (s_rot == 180) { x = (uint16_t)(PANEL_W - 1 - x); y = (uint16_t)(PANEL_H - 1 - y); }
            s_touchQ.add(TouchQueue::DOWN, (int16_t)x, (int16_t)y, now);
            // A press held back from LVGL (wake-only) still restarts the inactivity time.
            if (s_touchQ.holding()) lv_disp_trig_activity(NULL);
            break;
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    if (!s_touchReplaying) touch_sample(true);
    TouchQueue::Sample s;
    if (!s_touchQ.next(s)) s = s_touchQ.delivered();   // nothing new: same state as before
    data->point.x = s.x;
    data->point.y = s.y;
    data->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !s_touchQ.empty();
    s_touchReplaying = data->continue_reading;
}

namespace display {

bool begin() {
    Serial.println("[display] init EK79007 MIPI-DSI (1024x600, 2 lanes)...");
    backlight_write(0);   // dark until the first frame is in the framebuffer

    s_dsi = new Arduino_ESP32DSIPanel(DSI_HSYNC_PW, DSI_HSYNC_BP, DSI_HSYNC_FP,
                                      DSI_VSYNC_PW, DSI_VSYNC_BP, DSI_VSYNC_FP,
                                      DSI_DPI_CLK_HZ, DSI_LANE_MBPS);
    s_gfx = new Arduino_DSI_Display(PANEL_W, PANEL_H, s_dsi, 0 /*rotation*/, true /*auto flush*/,
                                    PIN_LCD_RST, EK79007_INIT,
                                    sizeof(EK79007_INIT) / sizeof(EK79007_INIT[0]));
    if (!s_gfx->begin()) {
        Serial.println("[display] gfx->begin() FAILED");
        return false;
    }
    s_gfx->fillScreen(RGB565_BLACK);
    backlight_write(BRIGHTNESS_DEFAULT);
    Serial.println("[display] panel up; init LVGL...");

    lv_init();

    const size_t buf_px = (size_t)PANEL_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_buf1) {
        Serial.println("[display] draw buffer allocation FAILED");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = PANEL_W;
    s_disp_drv.ver_res  = PANEL_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);
    s_ready = true;

    if (touch_begin()) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
        s_touchOk = true;
        Serial.println("[display] touch registered (" BOARD_NAME ")");
    }

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();
    Serial.println("[display] LVGL ready");
    return true;
}

void loop() {
    if (s_ready) lv_timer_handler();
}

// No per-pixel compositor on this board: LVGL draws the sweep (radar_view.cpp).
bool  sweepAvailable() { return false; }
void  sweepConfig(uint16_t, uint16_t, uint16_t, bool, bool) {}
void  sweepTick() {}
float sweepAngleDeg() { return 0.0f; }

void setBrightness(uint8_t v) {
    backlight_write(v);
    s_touchQ.setWakeOnly(v == 0);   // a dark panel takes a touch only as a wake-up
}

void setRotation(uint16_t degrees) {
    // Landscape layout: only "as mounted" or "upside down". Anything in the half-turn
    // nearer 180 counts as 180.
    const uint16_t d = (uint16_t)(degrees % 360);
    const uint16_t normalized = (d > 90 && d <= 270) ? 180 : 0;
    if (d != normalized)
        Serial.printf("[display] rotation %u not supported on the landscape panel; using %u\n",
                      (unsigned)d, (unsigned)normalized);
    if (normalized == s_rot) return;
    s_rot = normalized;
    if (s_gfx) {
        s_gfx->setRotation(normalized == 180 ? 2 : 0);
        s_gfx->fillScreen(RGB565_BLACK);
    }
    if (s_ready) lv_obj_invalidate(lv_scr_act());   // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

uint32_t inactiveMs() { return s_ready ? lv_disp_get_inactive_time(NULL) : 0; }

} // namespace display

#endif  // BOARD_P4_LCD7B
