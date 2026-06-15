#include "../../hal/time_hal.h"

// No RTC on this board (BoardCaps.has_rtc = false). Stub so shared code links;
// wall-clock time comes from the host epoch over BLE only.

void time_hal_init(void) {}
bool time_hal_now(struct tm *out) { (void)out; return false; }
void time_hal_set_epoch(uint32_t e) { (void)e; }
