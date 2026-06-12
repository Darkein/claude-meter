#include "volume.h"
#include "ui.h"
#include "hal/audio_hal.h"
#include <Preferences.h>
#include <Arduino.h>

// 4 levels: 0 = off/mute, 1 = low, 2 = med, 3 = high. Default (idx 2) = med,
// the amplitude the user signed off on. The level index is what we persist
// (not a raw amplitude) — same convention as brightness.
#define LEVELS_COUNT 4
#define DEFAULT_IDX  2

static uint8_t cur_idx = DEFAULT_IDX;

void volume_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t saved_idx = prefs.getUChar("vol_idx", 0xFF);
    prefs.end();

    if (saved_idx < LEVELS_COUNT) cur_idx = saved_idx;
    audio_hal_set_volume(cur_idx);
    ui_update_volume(cur_idx);
    Serial.printf("Volume init: idx=%u\n", cur_idx);
}

void volume_cycle(void) {
    cur_idx = (cur_idx + 1) % LEVELS_COUNT;

    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("vol_idx", cur_idx);
    prefs.end();

    audio_hal_set_volume(cur_idx);
    ui_update_volume(cur_idx);
    // Audible confirmation at the new level (skipped automatically when muted —
    // audio_hal_play early-returns at level 0).
    audio_hal_play(SND_ALERT);
    Serial.printf("Volume cycled: idx=%u\n", cur_idx);
}

uint8_t volume_get(void) { return cur_idx; }
