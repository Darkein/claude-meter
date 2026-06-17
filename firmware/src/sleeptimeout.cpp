#include "sleeptimeout.h"
#include "idle.h"
#include <Preferences.h>
#include <Arduino.h>

// Preset delays (ms) and their labels. 0 = never auto-sleep. Default idx 1
// (1 min) preserves the prior compile-time IDLE_TIMEOUT_MS behaviour.
static const uint32_t PRESETS_MS[] = {30000, 60000, 120000, 300000, 600000, 0};
static const char *const LABELS[]  = {"30s", "1m", "2m", "5m", "10m", "Never"};
#define PRESETS_COUNT (sizeof(PRESETS_MS) / sizeof(PRESETS_MS[0]))
#define DEFAULT_IDX 1

static uint8_t cur_idx = DEFAULT_IDX;

void sleeptimeout_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t saved = prefs.getUChar("slp_idx", 0xFF);
    prefs.end();

    if (saved < PRESETS_COUNT) cur_idx = saved;
    idle_set_timeout_ms(PRESETS_MS[cur_idx]);
    Serial.printf("Sleep timeout init: %s (idx=%u)\n", LABELS[cur_idx], cur_idx);
}

void sleeptimeout_set(uint8_t idx) {
    if (idx >= PRESETS_COUNT) idx = PRESETS_COUNT - 1;
    cur_idx = idx;

    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("slp_idx", cur_idx);
    prefs.end();

    idle_set_timeout_ms(PRESETS_MS[cur_idx]);
    Serial.printf("Sleep timeout set: %s (idx=%u)\n", LABELS[cur_idx], cur_idx);
}

uint8_t sleeptimeout_get(void) { return cur_idx; }
uint8_t sleeptimeout_count(void) { return PRESETS_COUNT; }

const char *sleeptimeout_label(uint8_t idx) {
    return (idx < PRESETS_COUNT) ? LABELS[idx] : "";
}
