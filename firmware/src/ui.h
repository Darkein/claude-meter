#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_USAGE,
    SCREEN_APPROVAL,   // remote tool-permission prompt (touch Allow/Deny)
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
