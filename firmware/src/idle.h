#pragma once
#include <stdbool.h>
#include <stdint.h>

void idle_init(void);
void idle_tick(void);
void idle_note_activity(void);

// Runtime auto-sleep delay (ms). Defaults to IDLE_TIMEOUT_MS; the Settings
// screen overrides it via sleeptimeout.{h,cpp}. A value of 0 means "never
// sleep". Persistence lives in sleeptimeout, not here.
void     idle_set_timeout_ms(uint32_t ms);
uint32_t idle_get_timeout_ms(void);

// Set the "awake" brightness target (0..255). idle owns display brightness
// (it fades between this and 0), so user brightness control routes through
// here. Applied immediately if the screen is currently fully awake; otherwise
// picked up by the next fade-in. See brightness.{h,cpp}.
void idle_set_awake_brightness(uint8_t level);

// Returns true if this press was consumed as a wake-up (caller MUST skip the
// button's normal action). Returns false when already awake — also notes the
// activity, so callers don't need a separate idle_note_activity() call.
bool idle_consume_wake_press(void);

// Touch should NOT count as activity (avoids accidental wakes from pets,
// sleeves, etc.). Callers use this to silently drop touch events while the
// panel is dark.
bool idle_is_asleep(void);

// Force the panel awake (fade in) regardless of current state, and reset the
// idle timer. Unlike idle_consume_wake_press(), this is for non-button wake
// sources (e.g. Claude activity over BLE) — no press-swallow semantics.
void idle_wake(void);

// Force the panel to sleep now (fade out) regardless of the idle timer. Used
// by the PWR button to toggle the screen off while awake. No-op if already
// asleep/fading out.
void idle_sleep_now(void);
