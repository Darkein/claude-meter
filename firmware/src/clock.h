#pragma once
#include <stdint.h>
#include <time.h>

// Shared wall-clock module. Holds the current time from two sources, in order
// of trust:
//   1. The host epoch pushed over BLE (clock_sync_epoch) — primary, corrects
//      drift on every poll. The daemon sends *local wall time* as an epoch, so
//      no timezone handling happens on the device.
//   2. The board RTC (clock_init seeds from time_hal at boot) — backend for
//      boards that have one, so the clock shows real time before the first poll
//      and survives a daemon outage / reboot.
// Between syncs the time is extrapolated with millis(). Boards without an RTC
// and before any poll report "no time" (clock_now → false).

void clock_init(void);                       // seed from the board RTC if present
void clock_sync_epoch(uint32_t wall_epoch);  // host pushed a fresh local-wall epoch
bool clock_now(struct tm *out);              // current time; false if never synced
