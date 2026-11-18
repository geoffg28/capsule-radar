// One-off maintenance sketch for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
//
// Updates the on-board ESP32-C6 (the WiFi co-processor, driven over SDIO by
// ESP-Hosted) to the firmware version this Arduino core's host side was built
// against. A C6 on older factory firmware answers simple calls but hangs on a
// WiFi scan, which freezes and then crashes the radar's setup portal.
//
// The C6 image is not compiled in: add_c6_image.py has the upload write it to
// flash at C6_IMAGE_OFFSET, and this sketch reads it back from there, checks its
// SHA-256, and streams it to the C6 with the core's hosted*Update() API. The C6
// writes into its spare OTA slot, so an interrupted update leaves the old
// firmware in place.
//
// Nothing happens until "UPDATE" is sent over serial (115200), so a reboot can
// never loop into a second update. See README.md next to this file.
#include <Arduino.h>
#include <WiFi.h>
#include "esp32-hal-hosted.h"
#include "esp_flash.h"
#include "mbedtls/sha256.h"

#if !defined(C6_IMAGE_OFFSET) || !defined(C6_IMAGE_SIZE) || !defined(C6_IMAGE_SHA256)
#error "build with env:esp32-p4-c6-update (add_c6_image.py defines the C6 image location)"
#endif

static constexpr size_t CHUNK = 1024;   // bytes per OTA write RPC

static bool readImage(uint32_t off, uint8_t *buf, size_t len) {
    return esp_flash_read(esp_flash_default_chip, buf, C6_IMAGE_OFFSET + off, len) == ESP_OK;
}

static bool verifyImage() {
    static uint8_t buf[CHUNK];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (uint32_t off = 0; off < C6_IMAGE_SIZE; off += CHUNK) {
        const size_t n = min((uint32_t)CHUNK, (uint32_t)C6_IMAGE_SIZE - off);
        if (!readImage(off, buf, n)) { mbedtls_sha256_free(&ctx); return false; }
        mbedtls_sha256_update(&ctx, buf, n);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    Serial.printf("[c6] image in flash sha256 %s\n", hex);
    return strcmp(hex, C6_IMAGE_SHA256) == 0;
}

static void printVersions() {
    uint32_t hM, hm, hp, sM, sm, sp;
    hostedGetHostVersion(&hM, &hm, &hp);
    hostedGetSlaveVersion(&sM, &sm, &sp);
    Serial.printf("[c6] host (P4) ESP-Hosted version : %lu.%lu.%lu\n", (unsigned long)hM, (unsigned long)hm, (unsigned long)hp);
    if (sM == 0 && sm == 0 && sp == 0)
        Serial.println("[c6] C6 firmware version         : unknown (it did not answer - old firmware)");
    else
        Serial.printf("[c6] C6 firmware version         : %lu.%lu.%lu\n", (unsigned long)sM, (unsigned long)sm, (unsigned long)sp);
}

static bool runUpdate() {
    static uint8_t buf[CHUNK];
    Serial.println("[c6] begin");
    if (!hostedBeginUpdate()) return false;
    int lastPct = -1;
    const uint32_t t0 = millis();
    for (uint32_t off = 0; off < C6_IMAGE_SIZE; off += CHUNK) {
        const size_t n = min((uint32_t)CHUNK, (uint32_t)C6_IMAGE_SIZE - off);
        if (!readImage(off, buf, n)) { Serial.printf("\n[c6] flash read failed at %lu\n", (unsigned long)off); return false; }
        if (!hostedWriteUpdate(buf, n)) { Serial.printf("\n[c6] write failed at %lu\n", (unsigned long)off); return false; }
        const int pct = (int)((uint64_t)(off + n) * 100 / C6_IMAGE_SIZE);
        if (pct / 5 != lastPct / 5) { Serial.printf("[c6] %3d%%\n", pct); lastPct = pct; }
    }
    Serial.printf("[c6] sent %lu bytes in %lu s\n", (unsigned long)C6_IMAGE_SIZE, (unsigned long)((millis() - t0) / 1000));
    Serial.println("[c6] end (C6 validates the image)");
    if (!hostedEndUpdate()) return false;
    Serial.println("[c6] activate");
    if (!hostedActivateUpdate()) return false;
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("=== ESP32-C6 co-processor firmware updater (P4 7B) ===");
    Serial.printf("[c6] bundled image: esp32c6-v%s.bin, %lu bytes at flash 0x%X\n",
                  C6_IMAGE_VERSION, (unsigned long)C6_IMAGE_SIZE, C6_IMAGE_OFFSET);

    if (!verifyImage()) {
        Serial.println("[c6] ERROR: image in flash does not match the build - re-upload this env. Stopping.");
        return;
    }
    Serial.println("[c6] image verified");

    // Bring up ESP-Hosted (SDIO link to the C6); this also queries the C6's version.
    WiFi.STA.begin();
    delay(1000);
    if (!hostedIsInitialized()) {
        Serial.println("[c6] ERROR: ESP-Hosted did not initialise - no link to the C6. Stopping.");
        return;
    }
    printVersions();

    if (!hostedHasUpdate()) {
        Serial.println("[c6] C6 already runs the host's version - nothing to do.");
        Serial.println("[c6] DONE: reflash the radar (pio run -e esp32-p4-lcd-7b -t upload).");
        return;
    }

    Serial.println("[c6] READY: send UPDATE to flash the C6 (anything else is ignored).");
    String line;
    while (true) {
        while (Serial.available()) {
            const char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                line.trim();
                if (line == "UPDATE") goto go;
                line = "";
            } else {
                line += c;
            }
        }
        delay(10);
    }
go:
    if (runUpdate()) {
        Serial.println("[c6] SUCCESS: C6 updated. Restarting to check the new version...");
    } else {
        Serial.println("[c6] FAILED: the C6 keeps its old firmware. Restarting.");
    }
    Serial.flush();
    delay(1500);
    ESP.restart();
}

void loop() { delay(1000); }
