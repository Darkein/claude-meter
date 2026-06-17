#pragma once
#include <stdint.h>

// User-controlled auto-sleep delay, persisted to NVS. Mirrors brightness/volume.
// Exposes a fixed table of presets the Settings screen renders; the last entry
// is "Never" (0 ms → the panel never auto-sleeps). The chosen delay routes
// through idle_set_timeout_ms().
void        sleeptimeout_init(void);          // load saved idx from NVS and apply
void        sleeptimeout_set(uint8_t idx);    // select preset, persist, apply
uint8_t     sleeptimeout_get(void);           // current preset index
uint8_t     sleeptimeout_count(void);         // number of presets
const char* sleeptimeout_label(uint8_t idx);  // human label, e.g. "2m" / "Never"
