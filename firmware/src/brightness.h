#pragma once
#include <stdint.h>

// User-controlled display brightness, persisted to NVS as a raw 0..255 PWM
// value. The Settings screen drives it continuously; the middle (PWR/BOOT)
// button short-press still steps through a few coarse presets via
// brightness_cycle(). idle owns the actual panel brightness, so the chosen
// level routes through idle_set_awake_brightness().

// Slider/PWM floor — never let the panel go fully black from the UI.
#define BRIGHTNESS_MIN 16

void    brightness_init(void);             // load saved value from NVS and apply
void    brightness_preview(uint8_t val);   // clamp + apply live, no NVS write (drag)
void    brightness_set(uint8_t val);       // preview + persist to NVS (release / button)
void    brightness_cycle(void);            // step to next coarse preset, persist, apply
uint8_t brightness_get(void);              // current PWM value (BRIGHTNESS_MIN..255)
