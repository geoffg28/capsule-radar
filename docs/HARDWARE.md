# Hardware

Capsule Radar supports two Waveshare round-AMOLED boards. Both use the same SoC and
the same 466×466 CO5300 panel, but their **pin maps have nothing in common** — flashing
one board's image onto the other gives a black screen and a silent I2C bus.

Pin maps live one-per-board in [`../src/boards/`](../src/boards/) and are selected by a
build flag (see [`../src/config.h`](../src/config.h)):

| Board | PlatformIO env | Build flag | Header |
|---|---|---|---|
| ESP32-S3-Touch-AMOLED-1.75 (reference) | `esp32-s3-amoled-175` | *(default)* | `board_amoled_175.h` |
| ESP32-S3-Touch-AMOLED-1.43 | `esp32-s3-amoled-143` | `-DBOARD_AMOLED_143` | `board_amoled_143.h` |

---

## ESP32-S3-Touch-AMOLED-1.75 (reference board)

- **MCU**: ESP32-S3R8 (Xtensa LX7 dual-core @240 MHz), 512 KB SRAM, **8 MB PSRAM**, **16 MB flash**.
- **Wireless**: 2.4 GHz WiFi (b/g/n) + Bluetooth 5 (LE), onboard antenna (IPEX option).
- **Display**: 1.75" AMOLED, **466×466**, driver **CO5300** over **QSPI**. Brightness via panel command (no PWM backlight line).
- **Touch**: **CST9217** capacitive, I2C.
- **IMU**: **QMI8658** 6-axis, I2C. **RTC**: **PCF85063**, I2C.
- **PMIC**: **AXP2101**, I2C — LiPo charge + rails.
- **Audio**: **ES8311** codec + onboard speaker; dual-mic array (ES7210 on the -C variant).
- **Storage**: microSD (TF). **Buttons**: PWR + BOOT. **GPS** (`-G` variant only): LC76G.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| LCD CS | 12 | | I2C SDA | 15 |
| LCD RST | 39 | | I2C SCL | 14 |
| LCD QSPI SCLK | 38 | | I2S MCLK | 42 |
| LCD QSPI D0–D3 | 4, 5, 6, 7 | | I2S BCLK | 9 |
| Touch INT | 11 | | I2S LRCLK | 45 |
| Touch RST | 40 | | I2S DOUT / DIN | 8 / 10 |
| Touch transform | mirror_x + mirror_y | | Speaker amp enable | 46 |

Panel column gap **6**, row gap 0, QSPI clock 80 MHz.
I2C: CST9217 `0x5A`, QMI8658 `0x6B`, PCF85063 `0x51`, AXP2101 `0x34`, ES8311 `0x18`.

Sources: the board's ESPHome definition, the Waveshare board definition in xiaozhi-esp32,
and a working Arduino_GFX port for this panel.

---

## ESP32-S3-Touch-AMOLED-1.43

Same 466×466 CO5300 panel, 8 MB PSRAM / 16 MB flash. Differences that matter:

- **Touch is an FT3168**, not a CST9217, at a different address and with a different protocol.
- **No AXP2101 and no ES8311** — no battery reporting and no alert pings. Both compile out
  via `BOARD_HAS_PMIC` / `BOARD_HAS_AUDIO`. This is not merely cosmetic: the 1.75's I2S
  BCLK/DIN (GPIO 9/10) are *this* board's QSPI CS and SCLK, so leaving audio enabled here
  would drive the panel's own bus lines.
- The FT3168 has **no reset line of its own**; it is tied to the panel reset (GPIO 21) and
  does not appear on I2C at all until the display has been initialised. `display::begin()`
  therefore must keep `gfx->begin()` ahead of `touch_begin()`.
- IMU and RTC are the same parts at the same addresses as the 1.75.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| LCD CS | 9 | | I2C SDA | 47 |
| LCD RST | 21 | | I2C SCL | 48 |
| LCD QSPI SCLK | 10 | | Touch INT / RST | not broken out |
| LCD QSPI D0–D3 | 11, 12, 13, 14 | | Touch transform | none (raw coords match) |
| Battery ADC | 4 (unused — see below) | | IMU INT / RTC INT | 8 / 15 |

Panel column gap **6** (same as the 1.75), row gap 0, QSPI clock 40 MHz (the vendor rate;
80 MHz is untested here).
I2C: FT3168 `0x38` (chip-id register `0xA3` reads `0x64`), QMI8658 `0x6B`, PCF85063 `0x51`.

Battery: there is a divided battery voltage on GPIO 4, but the divider ratio is not
documented, so the firmware reports "no battery" rather than an invented percentage.

**Sources.** Pins come from the Arduino-ESP32 core's own board variant
(`variants/waveshare_esp32_s3_touch_amoled_143/pins_arduino.h`, shared with the 1.64), and
each was then confirmed on hardware: an I2C scan on 47/48 answers `0x51` + `0x6B` + `0x38`,
and the panel drives correctly on CS=9 / SCK=10 / D0–D3=11–14 / RST=21. The touch orientation
was verified with an on-screen dot tracking a fingertip, and the column gap by nudging a
full-panel rim-ring test card one pixel at a time until it centred.

> Do **not** "correct" these against the Waveshare wiki pin table: that table is misaligned
> and lists an I2C SCL of GPIO 49, which does not exist on an ESP32-S3.

---

## ESP32-P4-WIFI6-Touch-LCD-7B (7" landscape, running on hardware)
Build with `-e esp32-p4-lcd-7b`; preview the layout on the desktop with `-e native-p4-7b`.
Pin map: `src/boards/board_p4_lcd_7b.h`.

A different SoC family from the boards above: ESP32-P4 (dual RISC-V, 32 MB PSRAM, 32 MB
flash) with an ESP32-C6 over SDIO for WiFi (esp-hosted; the Arduino core's `WiFi.h` hides
it). It needs Arduino core 3.3.x, so its env pins a newer pioarduino platform.

| Part | Detail |
|------|--------|
| Panel | EK79007, 1024x600 IPS, 2-lane MIPI-DSI at 1000 Mbit/s per lane, 52 MHz pixel clock. Reset GPIO33. |
| Backlight | LEDC PWM on GPIO32, 5 kHz, 10-bit, **active low** |
| I2C | SDA GPIO7, SCL GPIO8 (touch, ES8311 `0x18`, ES7210 `0x40`) |
| Touch | GT911 at `0x5D` on the board tested (`0x14` is probed too), polled; INT and RST are not wired to GPIOs in the vendor example. X and Y are mirrored relative to the panel (`TP_MIRROR_X/Y`). |
| WiFi | ESP32-C6 over SDIO (CLK 18, CMD 19, D0-D3 14-17, C6 reset 54), from the Arduino core's `esp32p4` variant |
| Audio | ES8311 + speaker (MCLK 13, BCLK 12, LRCK 10, DOUT 9, amp enable 53), ES7210 mics (DIN 11). Compiled out for now (`BOARD_HAS_AUDIO 0`). |
| Not fitted | PMIC, IMU, I2C RTC (their probes at boot find nothing, as expected) |

The pins come from Waveshare's Arduino examples for the board (`examples/arduino` in
github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B). Confirmed on a real board
(chip rev v3.x, November 2026): panel image, orientation and colours, GT911 touch, the SDIO
link to the C6 (WiFi setup portal, scan, joining a network, the live feed). **Not yet
exercised: the audio pins**, since audio is compiled out.

Serial: use the board's **UART** USB-C port (CH343). The build sends `Serial` to UART0, so the
native USB port (Espressif USB-Serial/JTAG) can flash but shows no log.

### The ESP32-C6 needs its firmware updated first
The C6 runs ESP-Hosted firmware that has to match the host side compiled into the Arduino
core (ESP-Hosted 2.12.x in core 3.3.x). The boards ship with older C6 firmware. Symptoms:

- `rpc_core: Response not received for [0x15e](Req_GetCoprocessorFwVersion)` on every boot;
- the setup hotspot starts, but **Configure WiFi** (a scan) hangs the C6, the display freezes
  and the board restarts a minute or so later.

Fix once per board, over the UART port, with the one-off env in `tools/c6_update/`
(it flashes the C6 image bundled in the core over the SDIO link; no internet needed):

```
pio run -e esp32-p4-c6-update -t upload --upload-port COMx
pio device monitor -b 115200 -p COMx      # type UPDATE when it says READY
pio run -e esp32-p4-lcd-7b -t upload --upload-port COMx
```

Details and the fallback (flashing the C6 through its own port) in `tools/c6_update/README.md`.

### ESP-Hosted quirks the firmware works around
- **Internal DMA RAM is the WiFi budget.** Every byte from the C6 lands in DMA-capable
  internal RAM. A 120 KB internal LVGL draw buffer once left ~3 KB of it, the scan reply could
  not be received, and the link wedged. The draw buffer lives in PSRAM; keep large buffers
  out of internal RAM on this board. `-DDEBUG_MEM=1` prints free DMA RAM every 5 s
  (about 85-90 KB with WiFi connected).
- **The WiFi driver can't keep credentials.** With ESP-Hosted the Arduino core forces its
  storage to RAM, so the radar keeps the SSID and password in its own NVS namespace
  (`capwifi`), restores them at boot, and erases them on Reset WiFi.
- **A hung C6 used to freeze the UI**, since every WiFi call from the loop waits out a 5 s
  RPC timeout, and ESP-Hosted's `rpc_wifi_ap_get_sta_list` dereferences NULL when the
  reply never comes. WiFiManager's debug output (which makes that call) is compiled out on
  this env (`WM_NODEBUG`); a small task probes the C6 every 15 s and restarts the board
  after two unanswered checks; the ADS-B task is on a 120 s task watchdog. A C6 that resets
  outright is handled by ESP-Hosted itself, which restarts the P4.

### Building next to the S3 boards
The S3 envs and this one pin different platform versions, but PlatformIO keeps a single
`framework-arduinoespressif32` package. Building one family replaces the core the other
needs, so the next build of the other family reinstalls it (slow), and **an S3 build and a
P4 build must not run at the same time** on one machine: the swap pulls files out from under
the build in progress.

Layout: the panel is split instead of round. `SCREEN_W/H` (683x600) is the radar pane on
the left two thirds, where the shared UI lays itself out exactly as on the round boards;
`UI_SIDE_LIST_W` (341) docks the aircraft list in the right third, so the list is always
on screen and the pane swipes radar / stats / weather. `PANEL_W/H` is the physical panel.

Driver notes (`display_dsi_ek79007.cpp`): no sweep compositor (LVGL draws the sweep),
rotation 0 or 180 only, brightness through the backlight PWM.

## Adding another board

1. Copy a header in `src/boards/`, fill in the pins **from the vendor demo or the Arduino
   core board variant** — never guess them.
2. Add an `[env:…]` in `platformio.ini` that `extends = amoled_common` and sets `-DBOARD_…`.
3. Set `BOARD_HAS_PMIC` / `BOARD_HAS_AUDIO` / `TOUCH_DRIVER_…` for what is actually fitted;
   add a touch driver alongside `touch_cst9217.cpp` / `touch_ft3168.cpp` if needed.
4. Confirm on hardware before committing: I2C scan, a full-panel test pattern for the gap,
   and a touch dot that lands under your fingertip.
5. Add the env to `.github/workflows/release.yml` so it ships with every release.

## Reference
- 1.75 wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- 1.43 wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43
- P4 7B docs: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-7B (examples: https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B)
- ESPHome device page (1.75 pin source): https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/
