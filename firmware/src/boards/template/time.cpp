#include "../../hal/time_hal.h"

// Optional RTC. If your board has one (e.g. PCF85063), set BOARD_HAS_RTC 1 in
// board.h, .has_rtc in caps.cpp, and implement these via its driver. Otherwise
// leave the stubs: wall-clock time then comes from the host epoch over BLE.

void time_hal_init(void) {}
bool time_hal_now(struct tm *out) { (void)out; return false; }
void time_hal_set_epoch(uint32_t e) { (void)e; }
