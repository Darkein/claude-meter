#pragma once
#include <stdint.h>
#include <stddef.h>

// Board-agnostic OTA receiver. Wraps the Arduino `Update` library (writes the
// inactive OTA partition; the bootloader only switches to it after a verified
// commit) plus an end-to-end SHA-256 check. No board-specific code — every
// board with a dual-OTA partition layout inherits this.
//
// Error codes reported back to the host (see ota_last_error()):
enum {
    OTA_ERR_NONE  = 0,
    OTA_ERR_BEGIN = 1,  // Update.begin() rejected the size (no slot / too big)
    OTA_ERR_WRITE = 2,  // a flash write failed mid-transfer
    OTA_ERR_SHA   = 3,  // SHA-256 of the received image != expected
    OTA_ERR_END   = 4,  // Update.end() failed (length mismatch / commit error)
};

// Start a session. size = total image bytes; sha256 = expected digest of the
// whole image. Returns false (and sets ota_last_error) if Update can't start.
bool ota_begin(uint32_t size, const uint8_t sha256[32]);

// Feed the next contiguous chunk into the receive buffer. Called from the BLE
// host task; it does NOT touch flash — it hands bytes to a RAM ring buffer and
// blocks (yielding) if the buffer is full, which propagates BLE link-layer
// backpressure so a fast host never overruns flash. Returns false on a stall or
// write error (session aborted, ota_last_error set).
bool ota_write(const uint8_t* data, size_t len);

// Commit buffered bytes to flash. Called every iteration from the main loop —
// this is where the actual Update.write() + SHA happen, decoupled from the BLE
// task. Bounded work per call so the loop isn't starved.
void ota_drain(void);

// Finalize: flush the buffer, verify SHA-256, commit, set as next boot
// partition. Returns true on success (caller should reboot). On failure aborts
// and sets ota_last_error; the current firmware stays bootable.
bool ota_end(void);

// Abort an in-progress session (e.g. BLE disconnect mid-transfer).
void ota_abort(void);

bool ota_active(void);          // true between ota_begin() and end/abort
int  ota_progress_pct(void);    // 0..100 of the active session, 0 when idle
int  ota_last_error(void);      // last OTA_ERR_* (cleared on ota_begin)
