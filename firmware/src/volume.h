#pragma once
#include <stdint.h>

// User-controlled notification-chime volume, persisted to NVS as a raw 0..255
// value applied straight to the audio HAL. The Settings screen drives it
// continuously; the KEY (right) button short-press steps through 4 coarse
// presets via volume_cycle(). The top-bar icon has 4 states, so the raw value
// is bucketed to 0..3 for ui_update_volume() internally.
void    volume_init(void);             // load saved value, apply to audio HAL + icon
void    volume_preview(uint8_t val);   // apply live (HAL + icon), no NVS write (drag)
void    volume_set(uint8_t val);       // preview + persist + confirmation chime
void    volume_cycle(void);            // step to next coarse preset, persist, apply
uint8_t volume_get(void);              // current raw value (0..255)
