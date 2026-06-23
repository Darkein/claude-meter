#include "soundtheme.h"
#include "hal/audio_hal.h"
#include <Preferences.h>
#include <Arduino.h>

// Theme labels, indexed 0..COUNT-1. Index 0 ("Retro") = the original
// synthesized sine chimes; the rest are sampled PCM themes bundled in flash
// (audio_samples.h on boards that have audio). Default idx 0 keeps the prior
// behaviour for existing users.
static const char *const LABELS[] = {"Retro", "Modern", "Bells", "Arcade"};
#define THEME_COUNT (sizeof(LABELS) / sizeof(LABELS[0]))
#define DEFAULT_IDX 0

static uint8_t cur_idx = DEFAULT_IDX;

void soundtheme_init(void) {
    Preferences prefs;
    prefs.begin("claude-meter", true);
    uint8_t saved = prefs.getUChar("snd_thm", 0xFF);
    prefs.end();

    if (saved < THEME_COUNT) cur_idx = saved;
    audio_hal_set_theme(cur_idx);
    Serial.printf("Sound theme init: %s (idx=%u)\n", LABELS[cur_idx], cur_idx);
}

void soundtheme_set(uint8_t idx) {
    if (idx >= THEME_COUNT) idx = THEME_COUNT - 1;
    cur_idx = idx;

    Preferences prefs;
    prefs.begin("claude-meter", false);
    prefs.putUChar("snd_thm", cur_idx);
    prefs.end();

    audio_hal_set_theme(cur_idx);
    // Audition the new theme at the current volume (skipped automatically when
    // muted — audio_hal_play early-returns at volume 0).
    audio_hal_play(SND_PERMISSION);
    Serial.printf("Sound theme set: %s (idx=%u)\n", LABELS[cur_idx], cur_idx);
}

uint8_t soundtheme_get(void) { return cur_idx; }
uint8_t soundtheme_count(void) { return THEME_COUNT; }

const char *soundtheme_label(uint8_t idx) {
    return (idx < THEME_COUNT) ? LABELS[idx] : "";
}
