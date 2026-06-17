#include "volume.h"
#include "ui.h"
#include "hal/audio_hal.h"
#include <Preferences.h>
#include <Arduino.h>

// Raw 0..255 volume applied directly to the codec amplitude. Persisted as
// "vol_val"; the prior 4-level "vol_idx" scheme is abandoned. Default ≈160 ≈
// the old "med" the user signed off on.
static uint8_t cur_val = 160;

// Coarse steps the KEY button cycles through: mute / low / med / max.
static const uint8_t PRESETS[] = {0, 85, 170, 255};
#define PRESETS_COUNT (sizeof(PRESETS) / sizeof(PRESETS[0]))

// Map the raw 0..255 value to one of the 4 top-bar icon states
// (0=off, 1=low, 2=med, 3=high).
static uint8_t icon_bucket(uint8_t val) {
    return val ? (uint8_t)(1 + (uint16_t)val * 3 / 256) : 0;
}

void volume_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint16_t saved = prefs.getUShort("vol_val", 0xFFFF);
    prefs.end();

    if (saved <= 255) cur_val = (uint8_t)saved;
    audio_hal_set_volume(cur_val);
    ui_update_volume(icon_bucket(cur_val));
    Serial.printf("Volume init: val=%u\n", cur_val);
}

void volume_preview(uint8_t val) {
    cur_val = val;
    audio_hal_set_volume(cur_val);
    ui_update_volume(icon_bucket(cur_val));
}

void volume_set(uint8_t val) {
    volume_preview(val);

    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUShort("vol_val", cur_val);
    prefs.end();

    // Audible confirmation at the new level (skipped automatically when muted —
    // audio_hal_play early-returns at 0).
    audio_hal_play(SND_ALERT);
    Serial.printf("Volume set: val=%u\n", cur_val);
}

void volume_cycle(void) {
    // Hop to the first preset strictly above the current value, wrapping to the
    // lowest (mute). Keeps the button's "tap to step, wrap around" feel.
    uint8_t next = PRESETS[0];
    for (uint8_t i = 0; i < PRESETS_COUNT; i++) {
        if (PRESETS[i] > cur_val) { next = PRESETS[i]; break; }
    }
    volume_set(next);
}

uint8_t volume_get(void) { return cur_val; }
