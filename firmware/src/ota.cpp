#include "ota.h"
#include <Arduino.h>
#include <Update.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// Decoupled receiver: the BLE host task only copies incoming bytes into this
// RAM ring buffer (fast, never blocks on flash); the main loop pulls from it
// and does the slow Update.write()/SHA. A full buffer blocks the producer,
// which stalls the BLE host task and applies link-layer backpressure — so the
// host is paced to flash speed without ever dropping a packet. Big enough to
// absorb multi-chunk bursts between drains. Internal SRAM (the C6 has no PSRAM
// but plenty of free SRAM here).
#ifndef OTA_RING_SIZE
#define OTA_RING_SIZE (64 * 1024)
#endif

static bool     s_active  = false;
static uint32_t s_total   = 0;
static uint32_t s_written = 0;   // bytes committed to flash (drives progress)
static int      s_error   = OTA_ERR_NONE;
static uint8_t  s_expected[32];
static mbedtls_sha256_context s_sha;
static StreamBufferHandle_t   s_sb = nullptr;

void ota_abort(void) {
    if (!s_active) return;
    Update.abort();
    mbedtls_sha256_free(&s_sha);
    if (s_sb) xStreamBufferReset(s_sb);
    s_active = false;
    s_total = s_written = 0;
}

bool ota_begin(uint32_t size, const uint8_t sha256[32]) {
    if (s_active) ota_abort();
    s_error = OTA_ERR_NONE;

    if (!s_sb) {
        // Created once and reused across sessions (avoids a large mid-run alloc
        // on a possibly-fragmented heap).
        s_sb = xStreamBufferCreate(OTA_RING_SIZE, 1);
        if (!s_sb) { s_error = OTA_ERR_BEGIN; return false; }
    } else {
        xStreamBufferReset(s_sb);
    }

    if (size == 0 || !Update.begin(size)) {
        Serial.printf("OTA: begin failed (%lu B): %s\n",
                      (unsigned long)size, Update.errorString());
        s_error = OTA_ERR_BEGIN;
        return false;
    }
    memcpy(s_expected, sha256, sizeof(s_expected));
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);   // 0 = SHA-256 (not SHA-224)
    s_total = size;
    s_written = 0;
    s_active = true;
    Serial.printf("OTA: begin, %lu bytes\n", (unsigned long)size);
    return true;
}

bool ota_write(const uint8_t* data, size_t len) {
    if (!s_active || !s_sb) return false;
    // Blocks (yields) until the drainer frees space. A timeout here means the
    // main loop stopped draining — treat as a fatal stall.
    size_t sent = xStreamBufferSend(s_sb, data, len, pdMS_TO_TICKS(5000));
    if (sent != len) {
        Serial.println("OTA: receive buffer stalled");
        s_error = OTA_ERR_WRITE;
        ota_abort();
        return false;
    }
    return true;
}

// Pull up to `cap` bytes from the ring and commit them to flash + SHA. Returns
// false (and aborts) on a flash write error.
static bool commit_buffered(size_t cap) {
    static uint8_t buf[4096];
    size_t done = 0;
    while (s_active && done < cap) {
        size_t n = xStreamBufferReceive(s_sb, buf, sizeof(buf), 0);  // non-blocking
        if (n == 0) break;
        if (Update.write(buf, n) != n) {
            Serial.printf("OTA: write failed: %s\n", Update.errorString());
            s_error = OTA_ERR_WRITE;
            ota_abort();
            return false;
        }
        mbedtls_sha256_update(&s_sha, buf, n);
        s_written += n;
        done += n;
    }
    return true;
}

void ota_drain(void) {
    if (!s_active || !s_sb) return;
    commit_buffered(32 * 1024);  // bounded so the main loop isn't starved
}

bool ota_end(void) {
    if (!s_active) return false;

    // Flush whatever the BLE task buffered but the loop hasn't committed yet.
    while (s_sb && xStreamBufferBytesAvailable(s_sb) > 0) {
        if (!commit_buffered(64 * 1024)) return false;  // write error -> aborted
    }

    uint8_t got[32];
    mbedtls_sha256_finish(&s_sha, got);
    mbedtls_sha256_free(&s_sha);

    if (memcmp(got, s_expected, sizeof(got)) != 0) {
        Serial.println("OTA: SHA-256 mismatch — keeping current firmware");
        Update.abort();
        s_active = false;
        s_error = OTA_ERR_SHA;
        return false;
    }
    if (!Update.end(true)) {
        Serial.printf("OTA: end failed: %s\n", Update.errorString());
        s_active = false;
        s_error = OTA_ERR_END;
        return false;
    }
    s_active = false;
    Serial.println("OTA: success — rebooting into new firmware");
    return true;
}

bool ota_active(void) { return s_active; }

int ota_progress_pct(void) {
    if (!s_active || s_total == 0) return 0;
    uint32_t p = (uint64_t)s_written * 100 / s_total;
    return p > 100 ? 100 : (int)p;
}

int ota_last_error(void) { return s_error; }
