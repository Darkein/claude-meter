#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_USAGE,
    SCREEN_CLOCK,      // wall-clock + date + battery; reached by swiping
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
// virtual order is [Usage, Clock] followed by one slot per pending approval.
// Wraps around. Called from the touch swipe gesture.
void ui_swipe(int dir);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
void ui_update_volume(uint8_t idx);  // 0=off,1=low,2=med,3=high
