#pragma once
// ---------------------------------------------------------------------------
//  Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
//  ESP32-P4NRW32 (dual RISC-V, 32 MB PSRAM in package) + 32 MB flash,
//  7" 1024x600 IPS over 2-lane MIPI-DSI (EK79007), GT911 capacitive touch,
//  ESP32-C6-MINI-1 over SDIO for WiFi 6 / BLE (esp-hosted), ES8311 codec + speaker,
//  ES7210 dual-mic ADC. No PMIC, no IMU, no I2C RTC chip.
//
//  Every pin, timing and init byte below is lifted from Waveshare's own Arduino examples
//  for this exact board (examples/arduino in
//  github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B: libraries/displays/displays_config.h,
//  i2c.h, gt911.h and 09_Audio_Playback), not guessed.
//
//  Booted on a real board (chip rev v3.x, November 2026): panel, backlight, GT911 touch,
//  the SDIO link to the C6 and the ES8311 speaker (every alert sound and the self-test
//  tone) are confirmed; the ES7210 mics are unused. The C6 needs its ESP-Hosted firmware
//  updated once per board first:
//  see docs/HARDWARE.md and tools/c6_update/.
// ---------------------------------------------------------------------------

#define BOARD_NAME          "ESP32-P4-WIFI6-Touch-LCD-7B"
#define BOARD_PIO_ENV       "esp32-p4-lcd-7b"

// ---------- Screen ----------
// PANEL_* is the physical panel. SCREEN_* is the radar pane: the area the shared radar UI
// (ui.cpp / radar_view.cpp) lays itself out in, exactly as it does on the round boards.
// On this landscape panel the radar pane is the left two thirds and the aircraft list is
// docked in the remaining third (UI_SIDE_LIST_W) instead of being a swipe-to view.
// The radar pane starts at the panel's top-left corner, so pane coordinates are panel
// coordinates and touch needs no offset.
#define PANEL_W             1024
#define PANEL_H             600
#define SCREEN_W            683            // radar pane: 2/3 of the panel width
#define SCREEN_H            600
#define SCREEN_CX           341            // scope centre, in the middle of the radar pane
#define SCREEN_CY           300
#define RADAR_R_OUTER_PX    288            // outer ring radius (12 px clear of the top/bottom edge)
#define UI_SIDE_LIST_W      (PANEL_W - SCREEN_W)   // 341: docked aircraft list, right third
// The 288 px scope has room for every airport's identifier, even at 50 NM (config.h).
#define AIRPORT_LABEL_UNTOWERED_NM_DEFAULT AIRPORT_LABEL_ALWAYS_NM

// ---------- Panel: EK79007 over MIPI-DSI ----------
// The DSI clock/data lanes are dedicated PHY pads, not GPIOs: there is no PIN_LCD_SCLK.
#define PIN_LCD_RST         33
#define DSI_LANE_MBPS       1000           // per lane, two lanes
#define DSI_DPI_CLK_HZ      52000000       // 52 MHz pixel clock -> 60 Hz
#define DSI_HSYNC_PW        10
#define DSI_HSYNC_BP        160
#define DSI_HSYNC_FP        160
#define DSI_VSYNC_PW        1
#define DSI_VSYNC_BP        23
#define DSI_VSYNC_FP        12
// The EK79007 register table itself lives next to the driver (display_dsi_ek79007.cpp).

// ---------- Backlight (LEDC PWM, ACTIVE LOW) ----------
#define PIN_LCD_BL          32
#define LCD_BL_PWM_FREQ     5000           // matches Waveshare's BSP
#define LCD_BL_PWM_BITS     10             // duty 0..1023
#define LCD_BL_ACTIVE_LOW   1              // output is inverted: full duty = full brightness

// ---------- Shared I2C (touch + ES8311 + ES7210) ----------
#define PIN_I2C_SDA         7
#define PIN_I2C_SCL         8

// ---------- Touch (GT911, polled) ----------
// Waveshare's example leaves the GT911 INT and RST lines unassigned and polls. The chip
// answers on 0x5D or 0x14 depending on how INT sat when it came out of reset, so the driver
// probes both.
#define TOUCH_DRIVER_GT911  1
#define I2C_ADDR_TOUCH      0x5D
#define I2C_ADDR_TOUCH_ALT  0x14
// The GT911 reports from the opposite corner to the panel's pixel (0,0): Waveshare's own
// driver for this board (gt911.cpp, "Match the LCD-7B BSP's native 1024x600 GT911
// orientation") runs the panel at rotation 0 with mirror_x = mirror_y = 1. Without both
// mirrors every tap lands at the point reflected through the screen centre.
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true
// Bring-up aid: print every press/lift (raw and mapped coordinates) and repeated I2C read
// failures on serial. Set to 0 once touch is confirmed on hardware.
#define TOUCH_LOG_EDGES     1

// ---------- Peripherals ----------
#define BOARD_HAS_PMIC      0
// ES8311 + speaker (NS4150B amp on PIN_AUDIO_PA). audio.cpp's init clocks the codec from
// BCLK, so no MCLK tuning is board-specific; the I2C bus is already up (touch_gt911.cpp).
#define BOARD_HAS_AUDIO     1
#define I2C_ADDR_CODEC      0x18           // ES8311
#define I2C_ADDR_MIC_ADC    0x40           // ES7210
#define PIN_I2S_MCLK        13
#define PIN_I2S_BCLK        12
#define PIN_I2S_LRCLK       10
#define PIN_I2S_DOUT        9              // P4 -> ES8311 (speaker)
#define PIN_I2S_DIN         11             // ES7210 -> P4 (mics)
#define PIN_AUDIO_PA        53             // speaker amp enable (high = on)

// Not fitted on this board. The shared IMU/RTC drivers probe these addresses, get no
// answer and stay off; nothing else on this I2C bus uses them.
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51

// The feed task does TLS on a different SoC and core build than the S3 boards; give it the
// larger stack the IDF-built LCD 2.1 uses until it has been measured on hardware.
#define ADSB_TASK_STACK_BYTES 32768
