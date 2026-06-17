#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

// True once the AMOLED controller has been put into sleep-in (panel powered
// down, not just brightness 0). Gates the matching wake so we don't issue
// redundant sleep-out commands (each carries ~240ms of settle delay).
static bool panel_slept = false;

// True between a manual PWR-off (idle_sleep_now) and the next genuine wake.
// Suppresses the on-USB auto-wake so the user can turn the screen off while
// plugged in; any real wake (button/touch, Claude activity) clears it.
static bool manual_sleep = false;

enum IdleState {
    STATE_AWAKE,
    STATE_FADING_OUT,
    STATE_ASLEEP,
    STATE_FADING_IN,
};

static IdleState state = STATE_AWAKE;
static uint32_t last_activity_ms = 0;
static uint32_t fade_started_ms  = 0;
static uint32_t fade_last_step_ms = 0;
static uint8_t  fade_from = DISPLAY_DEFAULT_BRIGHTNESS;
static uint8_t  fade_to   = 0;
static uint8_t  awake_brightness = DISPLAY_DEFAULT_BRIGHTNESS;  // user-set "full" level (brightness.cpp)
static uint32_t s_timeout_ms = IDLE_TIMEOUT_MS;                 // runtime auto-sleep delay; 0 = never (sleeptimeout.cpp)

static void apply_brightness(uint8_t b) {
    display_hal_set_brightness(b);
}

static void begin_fade(uint8_t to, uint32_t now) {
    // Waking: power the panel controller back on before the brightness ramp so
    // the first non-zero step actually lights pixels. Sleep-out is ~240ms; it
    // only runs on a genuine wake (callers gate begin_fade(awake) on asleep).
    if (to != 0) {
        manual_sleep = false;   // any wake clears a manual screen-off
        if (panel_slept) {
            display_hal_wake();
            panel_slept = false;
        }
    }
    fade_from = (to == 0) ? awake_brightness : 0;
    fade_to   = to;
    fade_started_ms = now;
    fade_last_step_ms = now;
}

void idle_init(void) {
    state = STATE_AWAKE;
    last_activity_ms = millis();
    apply_brightness(awake_brightness);
}

void idle_set_timeout_ms(uint32_t ms) {
    s_timeout_ms = ms;
    // Resetting the activity clock here means a freshly-chosen delay starts
    // counting from now, not from the last touch — no surprise instant sleep
    // when shortening the delay from the Settings screen.
    last_activity_ms = millis();
}

uint32_t idle_get_timeout_ms(void) {
    return s_timeout_ms;
}

void idle_set_awake_brightness(uint8_t level) {
    awake_brightness = level;
    // Apply now if fully awake so a button press is visible immediately;
    // during fades/sleep the next fade-in picks it up.
    if (state == STATE_AWAKE) apply_brightness(level);
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    if (state == STATE_FADING_IN) return;
    if (state == STATE_AWAKE) return;
    // Asleep/fading-out shouldn't reach here in normal flow (callers gate via
    // idle_consume_wake_press first), but if it does: trigger a wake.
    begin_fade(awake_brightness, last_activity_ms);
    state = STATE_FADING_IN;
}

bool idle_consume_wake_press(void) {
    if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
        uint32_t now = millis();
        last_activity_ms = now;
        begin_fade(awake_brightness, now);
        state = STATE_FADING_IN;
        return true;
    }
    if (state == STATE_FADING_IN) {
        // Mid-wake — still swallow this press; the user shouldn't get a
        // half-wake half-action surprise.
        last_activity_ms = millis();
        return true;
    }
    last_activity_ms = millis();
    return false;
}

bool idle_is_asleep(void) {
    return state == STATE_ASLEEP || state == STATE_FADING_OUT;
}

void idle_wake(void) {
    uint32_t now = millis();
    last_activity_ms = now;
    if (state == STATE_AWAKE || state == STATE_FADING_IN) return;
    begin_fade(awake_brightness, now);
    state = STATE_FADING_IN;
}

void idle_sleep_now(void) {
    if (state == STATE_ASLEEP || state == STATE_FADING_OUT) return;
    manual_sleep = true;   // user explicitly turned the screen off — keep it off on USB
    begin_fade(0, millis());
    state = STATE_FADING_OUT;
}

void idle_tick(void) {
    uint32_t now = millis();

    // While on USB power (if configured), don't sleep — and wake from sleep
    // when power comes back. Treats USB-in as continuous activity.
    if (!IDLE_SLEEP_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = now;
        // Don't fight a manual PWR-off: only the on-USB auto-wake is suppressed;
        // a button/touch or Claude-activity wake clears manual_sleep first.
        if (!manual_sleep && (state == STATE_ASLEEP || state == STATE_FADING_OUT)) {
            begin_fade(awake_brightness, now);
            state = STATE_FADING_IN;
        }
    }

    switch (state) {
    case STATE_AWAKE:
        if (s_timeout_ms != 0 && now - last_activity_ms >= s_timeout_ms) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
        }
        break;

    case STATE_FADING_OUT:
    case STATE_FADING_IN: {
        if (now - fade_last_step_ms < IDLE_FADE_STEP_MS) break;
        fade_last_step_ms = now;
        uint32_t dur = (state == STATE_FADING_OUT) ? IDLE_FADE_OUT_MS : IDLE_FADE_IN_MS;
        uint32_t elapsed = now - fade_started_ms;
        if (elapsed >= dur) {
            apply_brightness(fade_to);
            if (state == STATE_FADING_OUT) {
                state = STATE_ASLEEP;
                // Brightness is already 0; now power the controller down too.
                display_hal_sleep();
                panel_slept = true;
            } else {
                state = STATE_AWAKE;
            }
        } else {
            // Linear interpolation fade_from -> fade_to over dur ms.
            int32_t span = (int32_t)fade_to - (int32_t)fade_from;
            int32_t b = (int32_t)fade_from + (span * (int32_t)elapsed) / (int32_t)dur;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            apply_brightness((uint8_t)b);
        }
        break;
    }

    case STATE_ASLEEP:
        break;
    }
}
