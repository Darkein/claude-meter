#include "brightness.h"
#include "idle.h"
#include "idle_cfg.h"
#include <Preferences.h>
#include <Arduino.h>

// Raw 0..255 PWM value, clamped to [BRIGHTNESS_MIN..255] so the UI can never
// fully black the panel. Persisted as "brt_val"; the prior 4-level "brt_idx"
// scheme is abandoned (first boot just falls back to the default).
static uint8_t cur_val = DISPLAY_DEFAULT_BRIGHTNESS;

static uint8_t clamp_val(uint8_t v) {
    if (v < BRIGHTNESS_MIN) return BRIGHTNESS_MIN;
    return v;
}

void brightness_init(void) {
    Preferences prefs;
    prefs.begin("claude-meter", true);
    uint16_t saved = prefs.getUShort("brt_val", 0xFFFF);
    prefs.end();

    if (saved <= 255) cur_val = clamp_val((uint8_t)saved);
    idle_set_awake_brightness(cur_val);
    Serial.printf("Brightness init: val=%u\n", cur_val);
}

void brightness_preview(uint8_t val) {
    cur_val = clamp_val(val);
    idle_set_awake_brightness(cur_val);
}

void brightness_set(uint8_t val) {
    brightness_preview(val);

    Preferences prefs;
    prefs.begin("claude-meter", false);
    prefs.putUShort("brt_val", cur_val);
    prefs.end();
    Serial.printf("Brightness set: val=%u\n", cur_val);
}

uint8_t brightness_get(void) {
    return cur_val;
}
