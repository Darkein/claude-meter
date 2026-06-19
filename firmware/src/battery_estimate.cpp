#include "battery_estimate.h"
#include "hal/power_hal.h"
#include <Preferences.h>
#include <Arduino.h>

// Seconds taken per 1% of fuel-gauge movement, EMA-smoothed and persisted.
// 0 = not learned yet. Keys live in the shared "clawdmeter" NVS namespace.
static uint16_t spp_dis = 0; // discharge: seconds per 1% drop
static uint16_t spp_chg = 0; // charge:    seconds per 1% rise

// Anchor for the in-progress measurement (re-set on (un)plug / boot / noise).
static int      last_pct = -1;
static uint32_t last_ms  = 0;
static bool     last_on_power = false;

#define DT_MIN_MS 10000UL            // <10s per 1% = fuel-gauge jitter, ignore
#define DT_MAX_MS (4UL * 3600 * 1000) // >4h per 1% = sleep/stall gap, ignore

static void persist(const char *key, uint16_t v) {
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUShort(key, v);
    prefs.end();
}

void battery_estimate_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    spp_dis = prefs.getUShort("bat_spd", 0);
    spp_chg = prefs.getUShort("bat_spc", 0);
    prefs.end();
    Serial.printf("Battery estimate init: spp_dis=%us spp_chg=%us\n", spp_dis, spp_chg);
}

void battery_estimate_update(int pct, bool charging) {
    bool on_power = charging || power_hal_is_vbus_in();
    uint32_t now = millis();

    // No battery, first sample, or direction flipped: re-anchor without measuring
    // (the interval across a plug/unplug event is meaningless).
    if (pct < 0 || last_pct < 0 || on_power != last_on_power) {
        last_pct = pct;
        last_ms  = now;
        last_on_power = on_power;
        return;
    }
    if (pct == last_pct) return; // no step yet

    // Steps in the expected direction (rise while charging, drop while on battery).
    int steps = on_power ? (pct - last_pct) : (last_pct - pct);
    if (steps <= 0) { // moved the "wrong" way (noise) — re-anchor, don't learn
        last_pct = pct;
        last_ms  = now;
        return;
    }

    uint32_t dt_per = (now - last_ms) / (uint32_t)steps;
    last_pct = pct;
    last_ms  = now;
    if (dt_per < DT_MIN_MS || dt_per > DT_MAX_MS) return; // out of plausible range

    uint16_t sec = (uint16_t)(dt_per / 1000);
    if (sec == 0) sec = 1;
    uint16_t *spp = on_power ? &spp_chg : &spp_dis;
    uint16_t next = *spp ? (uint16_t)((*spp * 3 + sec) / 4) : sec; // EMA 0.75 / 0.25
    if (next != *spp) {
        *spp = next;
        persist(on_power ? "bat_spc" : "bat_spd", next);
    }
}

int battery_estimate_mins(void) {
    // Read live so a fresh (un)plug picks the right rate immediately.
    bool on_power = power_hal_is_charging() || power_hal_is_vbus_in();
    int pct = power_hal_battery_pct();
    if (pct < 0) return -1;
    if (on_power) {
        if (pct >= 100) return 0;          // full
        if (!spp_chg) return -1;
        return (int)((long)(100 - pct) * spp_chg / 60);
    }
    if (!spp_dis) return -1;
    return (int)((long)pct * spp_dis / 60);
}
