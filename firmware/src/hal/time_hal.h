#pragma once
#include <stdint.h>
#include <time.h>

// Real-time-clock abstraction. Optional per board (see BoardCaps.has_rtc).
//
// Only the C6 AMOLED-2.16 populates a PCF85063 RTC; boards without one provide
// a stub so shared code links everywhere. The RTC is a backend for the shared
// clock module (clock.{h,cpp}): it seeds wall-clock time at boot (before the
// first BLE poll) and keeps time across reboots / daemon outages. The primary
// time source is still the host epoch pushed over BLE.

void time_hal_init(void);             // bring up the RTC if present; no-op otherwise
bool time_hal_now(struct tm *out);    // fill *out from the RTC; false if absent or never set
void time_hal_set_epoch(uint32_t e);  // write wall-clock epoch to the RTC (no-op if absent)
