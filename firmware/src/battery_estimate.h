#pragma once
#include <stdint.h>

// Battery runtime estimate derived from the fuel-gauge %-change rate — the
// AXP2101 exposes no discharge-current method, so "time left" / "time to full"
// is inferred from how long a single 1% step takes. The learned seconds-per-1%
// is EMA-smoothed and persisted to NVS (mirrors brightness/volume/sleeptimeout),
// so an estimate is available the instant the device boots or is (un)plugged;
// only the first-ever battery use waits for one step.
void battery_estimate_init(void);                     // load persisted rates from NVS
void battery_estimate_update(int pct, bool charging); // feed on each pct/charging change
int  battery_estimate_mins(void);  // minutes: time-left if on battery, time-to-full if charging; -1 if unknown
