#pragma once
#include <stdint.h>

// User-controlled notification-chime volume, persisted to NVS. The KEY (right)
// button short-press cycles through the levels via volume_cycle(). Mirrors the
// brightness module. 4 levels: 0 = off/mute, 1 = low, 2 = med, 3 = high.
void    volume_init(void);    // load saved level from NVS and apply to audio HAL
void    volume_cycle(void);   // advance to next level, save, apply, refresh icon
uint8_t volume_get(void);     // current level index (0..3)
