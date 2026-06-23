#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_USAGE,
    SCREEN_CLOCK,      // wall-clock + date + battery; reached by swiping
    SCREEN_SESSIONS,   // multi-session dashboard (one row per live Claude session); reached by swiping
    SCREEN_SETTINGS,   // brightness / volume / sleep-delay sliders; toggled by tapping the logo
    SCREEN_APPROVAL,   // mirrors Claude Code's tool-permission prompt (display-only)
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
screen_t ui_get_current_screen(void);

// Advance the on-screen carousel by `dir` (+1 = next, -1 = previous). The
// virtual order is [Usage, Clock, Sessions] followed by one slot per pending
// approval. Wraps around. Called from the touch swipe gesture.
void ui_swipe(int dir);

// Pairing-hint state on the disconnected screen, driven by the BOOT hold-to-pair
// gesture in main.cpp. IDLE shows the bonds-based hint ("Waiting for host" /
// "Pairing…" + how-to text); ARMED shows "Release to pair"; OPEN shows the soft
// pairing window ("Pairing open") until it elapses or a host connects.
enum pair_hint_t { PAIR_HINT_IDLE, PAIR_HINT_ARMED, PAIR_HINT_OPEN };
void ui_set_pair_hint(pair_hint_t h);

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
void ui_update_volume(uint8_t idx);  // 0=off,1=low,2=med,3=high

// Full-screen OTA progress modal, driven from the main loop during a BLE
// firmware update. ui_ota_show(true) raises it over everything; set_pct paints
// the bar (0..100).
void ui_ota_show(bool show);
void ui_ota_set_pct(int pct);
