# ESP32-C6 co-processor update (ESP32-P4-WIFI6-Touch-LCD-7B)

The 7B's WiFi is an on-board ESP32-C6 running ESP-Hosted firmware, driven by the P4
over SDIO. The Arduino core's host side (ESP-Hosted 2.12.x in core 3.3.x) expects a
matching C6 firmware. Boards shipped with older C6 firmware boot and even start the
setup hotspot, but:

- `rpc_core: Response not received for [0x15e](Req_GetCoprocessorFwVersion)` at boot;
- tapping **Configure WiFi** in the portal (a WiFi scan) hangs the C6, every later
  WiFi call times out after 5 s (display frozen), and the P4 then panics in
  `rpc_wifi_ap_get_sta_list`.

This env flashes the C6 with the image bundled in the core
(`framework-arduinoespressif32-libs/hosted/esp32c6-v<version>.bin`) over the SDIO link.
No internet needed.

## Steps

Use the board's UART USB-C port (CH343) so you can see the log.

```
pio run -e esp32-p4-c6-update -t upload --upload-port COMx
pio device monitor -b 115200 -p COMx
```

The log prints the host and C6 versions and verifies the image. If an update is due it
waits for you to type `UPDATE` and press Enter. After it restarts it should report the
C6 on the host's version. Then put the radar back:

```
pio run -e esp32-p4-lcd-7b -t upload --upload-port COMx
```

If the C6's old firmware refuses the update commands, it keeps running what it had.
The fallback is flashing the C6 directly through its own programming port with BOOT
held (see Waveshare's ESP32-P4-WIFI6 FAQ).
