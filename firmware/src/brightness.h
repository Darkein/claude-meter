#pragma once
#include <stdint.h>

// User-controlled display brightness, persisted to NVS as a raw 0..255 PWM
// value. The Settings screen drives it continuously (the physical buttons no
// longer touch brightness). idle owns the actual panel brightness, so the
// chosen level routes through idle_set_awake_brightness().

// Slider/PWM floor — never let the panel go fully black from the UI.
#define BRIGHTNESS_MIN 16

void    brightness_init(void);             // load saved value from NVS and apply
void    brightness_preview(uint8_t val);   // clamp + apply live, no NVS write (drag)
void    brightness_set(uint8_t val);       // preview + persist to NVS (slider release)
uint8_t brightness_get(void);              // current PWM value (BRIGHTNESS_MIN..255)
