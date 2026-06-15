#include "clock.h"
#include "hal/time_hal.h"
#include <Arduino.h>

// synced_epoch is the wall-clock epoch at the instant synced_at_ms was stamped.
// have_time gates clock_now(): false until either the RTC or a BLE poll gives
// us a real time.
static uint32_t synced_epoch = 0;
static uint32_t synced_at_ms = 0;
static bool     have_time    = false;

void clock_init(void) {
    time_hal_init();
    struct tm tmv;
    if (time_hal_now(&tmv)) {
        // RTC holds local wall time; turn it back into the same epoch space the
        // daemon uses. The firmware never sets TZ, so newlib's mktime treats the
        // struct as UTC — the exact inverse of the gmtime_r used in clock_now(),
        // i.e. no timezone shift either way (timegm isn't available here).
        tmv.tm_isdst = 0;
        synced_epoch = (uint32_t)mktime(&tmv);
        synced_at_ms = millis();
        have_time = true;
    }
}

void clock_sync_epoch(uint32_t wall_epoch) {
    if (wall_epoch == 0)
        return;
    synced_epoch = wall_epoch;
    synced_at_ms = millis();
    have_time = true;
    time_hal_set_epoch(wall_epoch);  // no-op on boards without an RTC
}

bool clock_now(struct tm *out) {
    if (!have_time || !out)
        return false;
    uint32_t elapsed = (millis() - synced_at_ms) / 1000;
    time_t t = (time_t)(synced_epoch + elapsed);
    gmtime_r(&t, out);  // epoch is local wall time; gmtime = no extra shift
    return true;
}
