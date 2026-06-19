#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "hal/audio_hal.h"
#include "hal/power_hal.h"
#include "ble.h"
#include "clock.h"
#include "idle.h"
#include "brightness.h"
#include "volume.h"
#include "sleeptimeout.h"
#include "battery_estimate.h"
#include <time.h>

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_48);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout
{
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t *bt_title_font;
    const lv_font_t *bt_status_font;
    const lv_font_t *bt_device_font;
    const lv_font_t *bt_credit_1_font;
    const lv_font_t *bt_credit_2_font;

    // Settings screen
    int16_t set_row_y;      // top of the first settings row
    int16_t set_row_gap;    // vertical distance between consecutive rows
    int16_t set_slider_dy;  // slider offset below its row's name/value labels
    int16_t set_slider_h;   // slider track height (touch target)
    const lv_font_t *set_label_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps &c)
{
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460)
    {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font = &font_tiempos_48;
        L.bt_status_font = &font_styrene_48;
        L.bt_device_font = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.set_row_y = 110;
        L.set_row_gap = 115;
        L.set_slider_dy = 46;
        L.set_slider_h = 26;
        L.set_label_font = &font_styrene_28;
    }
    else
    {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font = &font_tiempos_34;
        L.bt_status_font = &font_styrene_28;
        L.bt_device_font = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.set_row_y = 92;
        L.set_row_gap = 100;
        L.set_slider_dy = 38;
        L.set_slider_h = 22;
        L.set_label_font = &font_styrene_20;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG THEME_BG
#define COL_PANEL THEME_PANEL
#define COL_TEXT THEME_TEXT
#define COL_DIM THEME_DIM
#define COL_ACCENT THEME_ACCENT
#define COL_GREEN THEME_GREEN
#define COL_AMBER THEME_AMBER
#define COL_RED THEME_RED
#define COL_BAR_BG THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t *usage_container;
static lv_obj_t *lbl_title;
static lv_obj_t *usage_group; // the two usage panels — shown when connected
static lv_obj_t *error_group;       // API-error message — shown when the daemon reports !ok
static lv_obj_t *lbl_error;         // the wrapped error text inside error_group
static lv_obj_t *pair_group;        // pairing hint — shown when disconnected
static lv_obj_t *pair_title;        // title label — "Waiting for host" vs "Pairing…"
static lv_obj_t *bar_session;
static lv_obj_t *lbl_session_pct;
static lv_obj_t *lbl_session_label;
static lv_obj_t *lbl_session_reset;
static lv_obj_t *bar_weekly;
static lv_obj_t *lbl_weekly_pct;
static lv_obj_t *lbl_weekly_label;
static lv_obj_t *lbl_weekly_reset;
static lv_obj_t *lbl_anim; // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t *battery_img;
static lv_obj_t *logo_img;
static lv_image_dsc_t battery_dscs[5]; // empty, low, medium, full, charging

// ---- Volume indicator (shared, on top, left of battery) ----
static lv_obj_t *volume_img;
static lv_image_dsc_t volume_dscs[4]; // off, low, med, high

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t *idle_group;                 // the "Zzz" idle screen
static uint32_t last_data_ms = 0;            // lv_tick when the last valid usage update landed
static bool data_received = false;           // any valid update since boot
static int view_state = -1;                  // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000; // usage counts as "live" within this window (daemon sends ~60s)

// ---- Clock screen (wall-clock + date + battery) ----
static lv_obj_t *clock_container;
static lv_obj_t *lbl_clock_time;
static lv_obj_t *lbl_clock_date;
static lv_obj_t *lbl_clock_batt;
static lv_obj_t *lbl_clock_status; // mirrors the usage-screen Claude-state line

// ---- Settings screen (brightness / volume / sleep-delay sliders) ----
static lv_obj_t *settings_container;
static lv_obj_t *sld_brightness, *lbl_brightness_val;
static lv_obj_t *sld_volume, *lbl_volume_val;     // volume row absent when !has_audio
static lv_obj_t *sld_sleep, *lbl_sleep_val;

// ---- Permission screen (mirrors Claude Code's permission prompt, info-only) ----
static lv_obj_t *approval_container;
static lv_obj_t *lbl_approval_count;  // "1 / 3" badge (hidden when count == 1)
static lv_obj_t *lbl_approval_tool;   // tool name, e.g. "Bash"
static lv_obj_t *lbl_approval_detail; // command / path / url

// ---- Cached live Claude Code state (from ui_update) ----
static claude_state_t s_claude_state = CLAUDE_IDLE;
static int s_approval_count = 0;                 // aq — true total (may exceed s_approval_n)
static Approval s_approvals[MAX_APPROVALS];      // q — bounded detail list
static uint8_t s_approval_n = 0;                 // entries filled in s_approvals
static uint8_t s_approval_view_idx = 0;          // which approval the screen shows
static uint32_t s_idle_since_ms = 0; // when CLAUDE_IDLE was entered ("Finished!" 10s window)
#define IDLE_FINISHED_MS 10000

// ---- Cached API-error state (from ui_update, daemon's ok/em fields) ----
static bool s_error_active = false;       // daemon reported a catchable API error
static char s_error_msg[96] = "";         // em — device-displayable message

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool s_ble_connected = false; // cached BLE connection state
static uint32_t connected_at_ms = 0; // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS 4000

static const char *const spinner_frames[] = {
    "\xC2\xB7",
    "\xE2\x9C\xBB",
    "\xE2\x9C\xBD",
    "\xE2\x9C\xB6",
    "\xE2\x9C\xB3",
    "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1)) // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260,
    130,
    130,
    130,
    130,
    260,
};

static const char *const anim_messages[] = {
    "Accomplishing",
    "Elucidating",
    "Perusing",
    "Actioning",
    "Enchanting",
    "Philosophising",
    "Actualizing",
    "Envisioning",
    "Pondering",
    "Baking",
    "Finagling",
    "Pontificating",
    "Booping",
    "Flibbertigibbeting",
    "Processing",
    "Brewing",
    "Forging",
    "Puttering",
    "Calculating",
    "Forming",
    "Puzzling",
    "Cerebrating",
    "Frolicking",
    "Reticulating",
    "Channelling",
    "Generating",
    "Ruminating",
    "Churning",
    "Germinating",
    "Scheming",
    "Clauding",
    "Hatching",
    "Schlepping",
    "Coalescing",
    "Herding",
    "Shimmying",
    "Cogitating",
    "Honking",
    "Shucking",
    "Combobulating",
    "Hustling",
    "Simmering",
    "Computing",
    "Ideating",
    "Smooshing",
    "Concocting",
    "Imagining",
    "Spelunking",
    "Conjuring",
    "Incubating",
    "Spinning",
    "Considering",
    "Inferring",
    "Stewing",
    "Contemplating",
    "Jiving",
    "Sussing",
    "Cooking",
    "Manifesting",
    "Synthesizing",
    "Crafting",
    "Marinating",
    "Thinking",
    "Creating",
    "Meandering",
    "Tinkering",
    "Crunching",
    "Moseying",
    "Transmuting",
    "Deciphering",
    "Mulling",
    "Unfurling",
    "Deliberating",
    "Mustering",
    "Unravelling",
    "Determining",
    "Musing",
    "Vibing",
    "Discombobulating",
    "Noodling",
    "Wandering",
    "Divining",
    "Percolating",
    "Whirring",
    "Doing",
    "Wibbling",
    "Effecting",
    "Wizarding",
    "Working",
    "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct)
{
    if (pct >= 80.0f)
        return COL_RED;
    if (pct >= 50.0f)
        return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char *buf, size_t len)
{
    if (mins < 0)
    {
        snprintf(buf, len, "---");
    }
    else if (mins < 60)
    {
        snprintf(buf, len, "Resets in %dm", mins);
    }
    else if (mins < 1440)
    {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    }
    else
    {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t *dsc, int w, int h, const uint8_t *data)
{
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void)
{
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

static void init_volume_icons(void)
{
    init_icon_dsc_rgb565a8(&volume_dscs[0], ICON_VOL_OFF_W, ICON_VOL_OFF_H, icon_volume_off_data);
    init_icon_dsc_rgb565a8(&volume_dscs[1], ICON_VOL_LOW_W, ICON_VOL_LOW_H, icon_volume_low_data);
    init_icon_dsc_rgb565a8(&volume_dscs[2], ICON_VOL_MED_W, ICON_VOL_MED_H, icon_volume_med_data);
    init_icon_dsc_rgb565a8(&volume_dscs[3], ICON_VOL_HIGH_W, ICON_VOL_HIGH_H, icon_volume_high_data);
}

// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t *parent, int y, const char *pill_text,
                             lv_obj_t **out_pct, lv_obj_t **out_pill,
                             lv_obj_t **out_bar, lv_obj_t **out_reset)
{
    lv_obj_t *panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t *parent)
{
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    pair_title = lv_label_create(pair_group);
    lv_label_set_text(pair_title, "Waiting for host");
    lv_obj_set_style_text_font(pair_title, L.bt_status_font, 0);
    lv_obj_set_style_text_color(pair_title, COL_TEXT, 0);
    lv_obj_align(pair_title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN); // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t *parent)
{
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t *creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature)
        lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN); // update_view_state decides
}

// API-error screen — shown when the daemon reports a catchable failure (not
// logged in, no internet, API down…). Full-screen like the pairing/idle hints,
// so we never paint stale numbers as if they were live. The wrapped message
// comes straight from the daemon (`em`), so new error kinds need no firmware
// change — they just display.
static void build_error_group(lv_obj_t *parent)
{
    error_group = lv_obj_create(parent);
    lv_obj_set_size(error_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(error_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(error_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(error_group, 0, 0);
    lv_obj_set_style_pad_all(error_group, 0, 0);
    lv_obj_clear_flag(error_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(error_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(error_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lbl_error = lv_label_create(error_group);
    lv_label_set_long_mode(lbl_error, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_error, L.scr_w - 80); // margin clears the rounded corners
    lv_obj_set_style_text_align(lbl_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_error, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_error, COL_AMBER, 0);
    lv_label_set_text(lbl_error, "");
    lv_obj_align(lbl_error, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(error_group, LV_OBJ_FLAG_HIDDEN); // update_view_state decides
}

static void init_usage_screen(lv_obj_t *scr)
{
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_GESTURE_BUBBLE); // swipe -> screen cb

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    build_pair_group(usage_container);
    build_idle_group(usage_container);
    build_error_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Clock Screen ========
//
// Big 24h HH:MM, French date below, battery % at the bottom. Reached by
// swiping. Time comes from the shared clock module (host epoch + millis, RTC
// seed on boards that have one); shows "--:--" until the first sync.

static void init_clock_screen(lv_obj_t *scr)
{
    clock_container = lv_obj_create(scr);
    lv_obj_set_size(clock_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(clock_container, 0, 0);
    lv_obj_set_style_bg_opa(clock_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_container, 0, 0);
    lv_obj_set_style_pad_all(clock_container, 0, 0);
    lv_obj_clear_flag(clock_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clock_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lbl_clock_time = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_time, "--:--");
    lv_obj_set_style_text_font(lbl_clock_time, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_clock_time, COL_TEXT, 0);
    lv_obj_align(lbl_clock_time, LV_ALIGN_CENTER, 0, -40);

    lbl_clock_date = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_date, "");
    lv_obj_set_style_text_font(lbl_clock_date, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_clock_date, COL_DIM, 0);
    lv_obj_align(lbl_clock_date, LV_ALIGN_CENTER, 0, 30);

    lbl_clock_batt = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_batt, "");
    lv_obj_set_style_text_font(lbl_clock_batt, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_clock_batt, COL_DIM, 0);
    lv_obj_align(lbl_clock_batt, LV_ALIGN_BOTTOM_MID, 0, -60);

    // Claude-state line — same look + content as the usage screen's lbl_anim,
    // driven by compute_status_text() from clock_refresh()/ui_tick_anim().
    lbl_clock_status = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_status, "");
    lv_obj_set_style_text_font(lbl_clock_status, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_clock_status, COL_ACCENT, 0);
    lv_obj_align(lbl_clock_status, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN); // shown via swipe
}

// Format a duration in minutes as a compact ETA: "~3h20" / "~45 min".
static void fmt_eta(char *buf, size_t n, int mins)
{
    if (mins >= 60)
        snprintf(buf, n, "~%dh%02d", mins / 60, mins % 60);
    else
        snprintf(buf, n, "~%d min", mins < 1 ? 1 : mins);
}

// Refresh the clock labels from the shared clock + battery. Called each loop
// from ui_tick_anim() while SCREEN_CLOCK is active.
static void clock_refresh(void)
{
    static const char *const jours[7] = {"dim.", "lun.", "mar.", "mer.",
                                         "jeu.", "ven.", "sam."};
    static const char *const mois[12] = {"janv.", "fevr.", "mars",  "avr.",
                                         "mai",   "juin",  "juil.", "aout",
                                         "sept.", "oct.",  "nov.",  "dec."};
    struct tm tmv;
    if (!clock_now(&tmv))
    {
        lv_label_set_text(lbl_clock_time, "--:--");
        lv_label_set_text(lbl_clock_date, "");
    }
    else
    {
        lv_label_set_text_fmt(lbl_clock_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        int wd = tmv.tm_wday % 7;
        int mo = tmv.tm_mon % 12;
        lv_label_set_text_fmt(lbl_clock_date, "%s %d %s", jours[wd], tmv.tm_mday, mois[mo]);
    }

    int pct = power_hal_battery_pct();
    if (pct < 0)
    {
        lv_label_set_text(lbl_clock_batt, "");
        return;
    }
    int mins = battery_estimate_mins();
    bool on_power = power_hal_is_charging() || power_hal_is_vbus_in();
    if (mins < 0)
        lv_label_set_text_fmt(lbl_clock_batt, "%d %%", pct);
    else
    {
        char eta[16];
        fmt_eta(eta, sizeof(eta), mins);
        if (on_power && mins == 0)
            lv_label_set_text_fmt(lbl_clock_batt, "%d %%   full", pct);
        else if (on_power)
            lv_label_set_text_fmt(lbl_clock_batt, "%d %%   full in %s", pct, eta);
        else
            lv_label_set_text_fmt(lbl_clock_batt, "%d %%   %s", pct, eta);
    }
}

// ======== Settings Screen ========
//
// Brightness / volume / sleep-delay sliders. Reached by swiping. Each slider
// previews live on drag (LV_EVENT_VALUE_CHANGED) and persists on release
// (LV_EVENT_RELEASED) so a continuous drag never hammers NVS. The volume row is
// built only when the board has real audio hardware (board_caps().has_audio).
// Sliders own their own touch, so a drag on a slider adjusts it while a swipe
// over empty area bubbles up and changes screens.

static lv_obj_t *make_slider(lv_obj_t *parent, int x, int y, int w, int h,
                             int min, int max, int val)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB); // enlarge the knob beyond the track
    // Extend the touch hit-area past the visible track so a near-miss still grabs
    // the slider instead of bubbling up to the screen-swipe gesture.
    lv_obj_set_ext_click_area(s, 18);
    return s;
}

// Build one "Name ........ value" row with a full-width slider underneath.
// Returns the slider; *out_val receives the right-aligned value label.
static lv_obj_t *make_setting_row(lv_obj_t *parent, int y, const char *name,
                                  lv_obj_t **out_val, int min, int max, int val)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, L.set_label_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_pos(lbl, L.margin, y);

    *out_val = lv_label_create(parent);
    lv_obj_set_style_text_font(*out_val, L.set_label_font, 0);
    lv_obj_set_style_text_color(*out_val, COL_DIM, 0);
    lv_obj_align(*out_val, LV_ALIGN_TOP_RIGHT, -L.margin, y);

    return make_slider(parent, L.margin, y + L.set_slider_dy, L.content_w, L.set_slider_h, min, max, val);
}

static void brightness_slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    lv_label_set_text_fmt(lbl_brightness_val, "%d%%", v * 100 / 255);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        brightness_set((uint8_t)v); // persist
    else
        brightness_preview((uint8_t)v); // live, no NVS
}

static void volume_slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    lv_label_set_text_fmt(lbl_volume_val, "%d%%", v * 100 / 255);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        volume_set((uint8_t)v); // persist + confirmation chime
    else
        volume_preview((uint8_t)v); // live, no NVS
}

static void sleep_slider_cb(lv_event_t *e)
{
    int idx = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    lv_label_set_text(lbl_sleep_val, sleeptimeout_label((uint8_t)idx));
    // Sleep delay has no live effect to preview, so only act on release.
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        sleeptimeout_set((uint8_t)idx);
}

static void init_settings_screen(lv_obj_t *scr)
{
    settings_container = lv_obj_create(scr);
    lv_obj_set_size(settings_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_container, 0, 0);
    lv_obj_set_style_bg_opa(settings_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_set_style_pad_all(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_GESTURE_BUBBLE); // empty-area swipe -> screen cb

    lv_obj_t *title = lv_label_create(settings_container);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    int y = L.set_row_y;

    int b = brightness_get();
    sld_brightness = make_setting_row(settings_container, y, "Brightness",
                                      &lbl_brightness_val, BRIGHTNESS_MIN, 255, b);
    lv_label_set_text_fmt(lbl_brightness_val, "%d%%", b * 100 / 255);
    lv_obj_add_event_cb(sld_brightness, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sld_brightness, brightness_slider_cb, LV_EVENT_RELEASED, NULL);
    y += L.set_row_gap;

    if (board_caps().has_audio)
    {
        int v = volume_get();
        sld_volume = make_setting_row(settings_container, y, "Volume",
                                      &lbl_volume_val, 0, 255, v);
        lv_label_set_text_fmt(lbl_volume_val, "%d%%", v * 100 / 255);
        lv_obj_add_event_cb(sld_volume, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sld_volume, volume_slider_cb, LV_EVENT_RELEASED, NULL);
        y += L.set_row_gap;
    }

    int si = sleeptimeout_get();
    sld_sleep = make_setting_row(settings_container, y, "Sleep after",
                                 &lbl_sleep_val, 0, sleeptimeout_count() - 1, si);
    lv_label_set_text(lbl_sleep_val, sleeptimeout_label((uint8_t)si));
    lv_obj_add_event_cb(sld_sleep, sleep_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sld_sleep, sleep_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN); // shown via swipe
}

// Re-sync the sliders + value labels to the live values. Called on screen entry
// AND every loop while Settings is showing (from ui_tick_anim) so changes made
// via the physical buttons (brightness_cycle / volume_cycle) appear live. The
// per-field change guard means we only touch LVGL when a value actually moved,
// so the per-loop call costs nothing and doesn't fight an in-progress drag
// (a drag updates the underlying value to match the slider, so nothing changes).
static void settings_refresh(void)
{
    if (!settings_container)
        return;
    static int last_b = -1, last_v = -1, last_s = -1;

    int b = brightness_get();
    if (b != last_b)
    {
        last_b = b;
        lv_slider_set_value(sld_brightness, b, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_brightness_val, "%d%%", b * 100 / 255);
    }
    if (sld_volume)
    {
        int v = volume_get();
        if (v != last_v)
        {
            last_v = v;
            lv_slider_set_value(sld_volume, v, LV_ANIM_OFF);
            lv_label_set_text_fmt(lbl_volume_val, "%d%%", v * 100 / 255);
        }
    }
    int si = sleeptimeout_get();
    if (si != last_s)
    {
        last_s = si;
        lv_slider_set_value(sld_sleep, si, LV_ANIM_OFF);
        lv_label_set_text(lbl_sleep_val, sleeptimeout_label((uint8_t)si));
    }
}

// ======== Approval Screen ========
//
// Shown automatically when the daemon reports a pending tool-permission prompt
// (approval_count > 0). Info-only: it mirrors what Claude Code's own prompt
// asks (the tool + the command/path/url) so a glance at the device tells you
// what's blocked. The decision is still made on the laptop — no buttons here.

static void init_approval_screen(lv_obj_t *scr)
{
    approval_container = lv_obj_create(scr);
    lv_obj_set_size(approval_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(approval_container, 0, 0);
    lv_obj_set_style_bg_opa(approval_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(approval_container, 0, 0);
    lv_obj_set_style_pad_all(approval_container, 0, 0);
    lv_obj_clear_flag(approval_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(approval_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Header: "Permission" + optional "1 / N" queue badge.
    lbl_approval_count = lv_label_create(approval_container);
    lv_label_set_text(lbl_approval_count, "Permission");
    lv_obj_set_style_text_font(lbl_approval_count, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_approval_count, COL_DIM, 0);
    // Center the header text against the 48px top-bar icons (battery/volume),
    // whose vertical center sits at title_y + 24. A ~24px font centered there
    // needs its top at title_y + 12.
    lv_obj_align(lbl_approval_count, LV_ALIGN_TOP_MID, 0, L.title_y + 12);

    // Tool name, large and centered.
    lbl_approval_tool = lv_label_create(approval_container);
    lv_label_set_text(lbl_approval_tool, "");
    lv_obj_set_style_text_font(lbl_approval_tool, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_approval_tool, COL_TEXT, 0);
    lv_obj_set_style_text_align(lbl_approval_tool, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_approval_tool, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_approval_tool, L.content_w);
    lv_obj_align(lbl_approval_tool, LV_ALIGN_CENTER, 0, -30);

    // Detail (command / path / url), smaller, dimmed, below the tool name.
    lbl_approval_detail = lv_label_create(approval_container);
    lv_label_set_text(lbl_approval_detail, "");
    lv_obj_set_style_text_font(lbl_approval_detail, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_approval_detail, COL_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_approval_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_approval_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_approval_detail, L.content_w);
    lv_obj_align(lbl_approval_detail, LV_ALIGN_CENTER, 0, 50);

    lv_obj_add_flag(approval_container, LV_OBJ_FLAG_HIDDEN); // ui_update decides
}

// Refresh the permission-screen labels from the approval the user is viewing
// (s_approval_view_idx into the cached queue).
static void approval_refresh_labels(void)
{
    if (!lbl_approval_tool)
        return;

    // Clamp the view index to the live queue (it can shrink as prompts resolve).
    if (s_approval_n == 0)
        s_approval_view_idx = 0;
    else if (s_approval_view_idx >= s_approval_n)
        s_approval_view_idx = s_approval_n - 1;

    const Approval *a = (s_approval_n > 0) ? &s_approvals[s_approval_view_idx] : nullptr;
    const char *tool = a ? a->tool : "";
    const char *detail = a ? a->detail : "";

    // The daemon sends the raw trigger as tool_name. AskUserQuestion and
    // ExitPlanMode aren't permission prompts — they block on a user choice /
    // plan approval — so show natural wording for each and keep the real tool
    // name otherwise.
    bool is_question = (strcmp(tool, "AskUserQuestion") == 0);
    bool is_plan = (strcmp(tool, "ExitPlanMode") == 0);
    const char *title = is_plan     ? "Approve plan?"
                        : is_question ? "Claude is asking"
                                      : (tool[0] ? tool : "Tool");
    lv_label_set_text(lbl_approval_tool, title);
    lv_label_set_text(lbl_approval_detail, detail);

    // Center the title on the Y axis when there's no detail line under it
    // (typical for a question); nudge it up to make room when a detail shows.
    lv_obj_align(lbl_approval_tool, LV_ALIGN_CENTER, 0, detail[0] ? -30 : 0);

    // Badge shows the viewed position within the true total (aq may exceed the
    // bounded queue we actually received).
    int pos = s_approval_view_idx + 1;
    if (s_approval_count > 1)
        lv_label_set_text_fmt(lbl_approval_count, "Permission  %d / %d", pos, s_approval_count);
    else
        lv_label_set_text(lbl_approval_count,
                          is_plan ? "Plan" : is_question ? "Question" : "Permission");
}

// ======== Public API ========

// Screen-level swipe handler. LVGL emits LV_EVENT_GESTURE on the active screen
// after the indev decodes a fling. Left->right (LV_DIR_RIGHT) = next screen.
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_RIGHT)
        ui_swipe(+1);
    else if (d == LV_DIR_LEFT)
        ui_swipe(-1);
}

// A tap in the top-left corner toggles the Settings screen: first tap enters
// from whatever screen we were on, second tap returns there. Settings is NOT
// part of the swipe carousel. The hit-zone is a transparent corner button
// (corner_hit) fixed at the logical top-left. Boards that rotate the display
// un-rotate touch back into this logical frame in their touch driver, so the
// zone (like the logo it sits under) tracks the viewer in every orientation.
#define CORNER_ZONE_PX 120
static lv_obj_t *corner_hit;
static screen_t settings_return_screen = SCREEN_USAGE;
static void settings_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (current_screen == SCREEN_SETTINGS)
        ui_show_screen(settings_return_screen);
    else
    {
        settings_return_screen = current_screen;
        ui_show_screen(SCREEN_SETTINGS);
    }
}

void ui_init(void)
{
    compute_layout(board_caps());

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();
    init_volume_icons();

    init_usage_screen(scr);
    init_clock_screen(scr);
    init_settings_screen(scr);
    init_approval_screen(scr);

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

    // Volume icon sits just left of the battery (48px wide + 12px gap).
    volume_img = lv_image_create(scr);
    lv_image_set_src(volume_img, &volume_dscs[2]);  // med default; volume_init() sets the real level
    lv_obj_set_pos(volume_img, L.scr_w - 48 - L.margin - 48 - 12, L.title_y);

    // Transparent Settings hit-zone, created last so it sits on top of every
    // screen. Parked over the current physically-top-left corner (the logo's
    // corner when upright). Swipes that start on it still bubble to the screen.
    corner_hit = lv_obj_create(scr);
    lv_obj_set_size(corner_hit, CORNER_ZONE_PX, CORNER_ZONE_PX);
    lv_obj_set_style_bg_opa(corner_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(corner_hit, 0, 0);
    lv_obj_set_style_pad_all(corner_hit, 0, 0);
    lv_obj_clear_flag(corner_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(corner_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(corner_hit, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(corner_hit, settings_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_pos(corner_hit, 0, 0); // logical top-left, over the logo
}

// React to a change in the cached Claude-state / approval-queue: refresh the
// permission-screen labels and auto-switch into / out of SCREEN_APPROVAL.
static screen_t approval_return_screen = SCREEN_USAGE;
static void apply_claude_state(void)
{
    if (s_approval_count > 0)
    {
        approval_refresh_labels();
        // Auto-raise the approval screen.
        if (current_screen != SCREEN_APPROVAL)
        {
            approval_return_screen = current_screen;
            ui_show_screen(SCREEN_APPROVAL);
        }
    }
    else if (current_screen == SCREEN_APPROVAL)
    {
        ui_show_screen(approval_return_screen);
    }
}

void ui_update(const UsageData *data)
{
    if (!data->valid)
        return;

    // Cache live Claude-state (drives the status line + approval screen) and
    // react before the usage-bar early bookkeeping.
    if (data->claude_state == CLAUDE_IDLE && s_claude_state != CLAUDE_IDLE)
        s_idle_since_ms = lv_tick_get(); // stamp the IDLE -> "Finished!" 10s window

    // Notification chimes on a state transition (edge-triggered so the daemon's
    // periodic re-send of the same cs doesn't re-beep). No-op on boards without
    // a speaker. WAITING / QUESTION → attention chime; turn-finished → done chime.
    if (data->claude_state != s_claude_state)
    {
        if (data->claude_state == CLAUDE_WAITING || data->claude_state == CLAUDE_QUESTION)
            audio_hal_play(SND_ALERT);
        else if (data->claude_state == CLAUDE_IDLE && s_claude_state == CLAUDE_WORKING)
            audio_hal_play(SND_DONE);

        // Wake the panel on any Claude-state edge so the user sees what just
        // changed (turn finished, attention needed, work started).
        if (data->claude_state != CLAUDE_NONE)
            idle_wake();
    }

    // Keep the panel awake while Claude is actively doing something. The daemon
    // re-sends the same cs periodically, so this resets the idle timer on every
    // update and the screen won't sleep mid-work.
    if (data->claude_state == CLAUDE_WORKING ||
        data->claude_state == CLAUDE_WAITING ||
        data->claude_state == CLAUDE_QUESTION)
        idle_note_activity();

    s_claude_state = data->claude_state;
    s_approval_count = data->approval_count;
    s_approval_n = data->approval_n;
    for (uint8_t i = 0; i < s_approval_n; i++)
        s_approvals[i] = data->approvals[i];
    apply_claude_state();

    last_data_ms = lv_tick_get(); // a valid update just landed → dot goes green
    data_received = true;

    // API-error overlay: when the daemon reports !ok with a message, latch it so
    // update_view_state() can swap in the error screen. A subsequent ok=true
    // update clears it and the usage bars return.
    bool err = !data->ok && data->error_msg[0] != '\0';
    if (err)
        strlcpy(s_error_msg, data->error_msg, sizeof(s_error_msg));
    if (err != s_error_active ||
        (err && lbl_error && strcmp(lv_label_get_text(lbl_error), s_error_msg) != 0))
    {
        s_error_active = err;
        if (err && lbl_error)
            lv_label_set_text(lbl_error, s_error_msg);
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void)
{
    if (!usage_group || !pair_group || !idle_group)
        return;
    int v;
    bool fresh = data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS;
    if (!s_ble_connected)
    {
        v = 0; // pairing hint
    }
    else if (fresh && s_error_active)
    {
        v = 3; // API error (not logged in, offline, API down…)
    }
    else if (fresh)
    {
        v = 2; // live usage
    }
    else
    {
        v = 1; // idle / Zzz
    }
    // Refresh the pairing title whenever bond state flips, even with no view
    // change — the 3s-hold can clear bonds while already on the pair screen.
    // Bonds present => a host paired before, just not connected (e.g. laptop
    // asleep). No bonds => fresh/cleared, waiting for first pairing.
    if (v == 0 && pair_title)
    {
        static int last_bonds = -1;
        int bonds = ble_has_bonds() ? 1 : 0;
        if (bonds != last_bonds)
        {
            last_bonds = bonds;
            lv_label_set_text(pair_title,
                              bonds ? "Waiting for host" : "Pairing\xE2\x80\xA6");
        }
    }
    if (v == view_state)
        return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    if (error_group)
        lv_obj_add_flag(error_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *shown = v == 0 ? pair_group : v == 1 ? idle_group
                            : v == 3 ? error_group
                                     : usage_group;
    if (shown)
        lv_obj_clear_flag(shown, LV_OBJ_FLAG_HIDDEN);
    // The bottom status line is noise under a full error message — hide it on
    // the error view, restore it everywhere else.
    if (lbl_anim)
    {
        if (v == 3)
            lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
    }
}

// Advance the spinner/msg timers and compute the current status string into buf.
// Returns true when the line should render animated (accent color + spinner).
// Reads view_state, so callers must run update_view_state() first. Shared by the
// usage screen (lbl_anim) and the clock screen (lbl_clock_status).
static bool compute_status_text(char *buf, size_t n)
{
    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS)
    {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }
    // Gate only the spinner-frame advance — the text is always produced so a
    // freshly-shown label (e.g. just after a screen switch) renders immediately.
    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx])
    {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);
    }

    // Status text by priority, driven by live Claude Code state when connected.
    // (A pending approval is surfaced on SCREEN_APPROVAL, not here.)
    // animated => spinner glyph + trailing "…" + rotating verbs / alternation.
    // Static states ("Finished !", "idle") render plain white, no spinner.
    const char *text;
    bool animated = true;
    if (!s_ble_connected)
    {
        text = "Waiting"; // advertising / waiting for a host connection
    }
    else if (view_state == 1)
    { // stale data — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    }
    else if (now - connected_at_ms < 5000)
    {
        text = "Connected";
    }
    else if (s_claude_state == CLAUDE_QUESTION)
    {
        text = "Your turn"; // blocked on AskUserQuestion / elicitation
        animated = false;
    }
    else if (s_claude_state == CLAUDE_IDLE && (now - s_idle_since_ms) < IDLE_FINISHED_MS)
    {
        text = "Finished !"; // a session just finished — shown for 10s, static white
        animated = false;
    }
    else if (s_claude_state == CLAUDE_IDLE || s_claude_state == CLAUDE_NONE)
    {
        text = "idle"; // no active work — static white
        animated = false;
    }
    else
    { // WORKING — rotate the whimsical verbs
        text = anim_messages[anim_msg_idx];
    }

    if (animated)
        snprintf(buf, n, "%s %s\xE2\x80\xA6", spinner_frames[anim_spinner_idx], text);
    else
        snprintf(buf, n, "%s", text); // plain, no spinner / ellipsis
    return animated;
}

void ui_tick_anim(void)
{
    char buf[80];
    if (current_screen == SCREEN_CLOCK)
    {
        clock_refresh();
        update_view_state(); // keep view_state fresh for the status line
        bool animated = compute_status_text(buf, sizeof(buf));
        lv_obj_set_style_text_color(lbl_clock_status, animated ? COL_ACCENT : COL_TEXT, 0);
        lv_label_set_text(lbl_clock_status, buf);
        return;
    }
    if (current_screen == SCREEN_SETTINGS)
    {
        settings_refresh(); // reflect physical-button changes live
        return;
    }
    if (current_screen != SCREEN_USAGE)
        return;
    update_view_state();
    if (view_state == 1)
        splash_mini_tick(); // animate the sleeping creature on the idle screen

    bool animated = compute_status_text(buf, sizeof(buf));
    lv_obj_set_style_text_color(lbl_anim, animated ? COL_ACCENT : COL_TEXT, 0);
    lv_label_set_text(lbl_anim, buf);
}

static void apply_battery_visibility(void)
{
    if (!battery_img)
        return;
    lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_screen(screen_t screen)
{
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (clock_container)
        lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
    if (settings_container)
        lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    if (approval_container)
        lv_obj_add_flag(approval_container, LV_OBJ_FLAG_HIDDEN);

    switch (screen)
    {
    case SCREEN_USAGE:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_CLOCK:
        lv_obj_clear_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
        clock_refresh(); // paint immediately so the swipe-in isn't blank
        break;
    case SCREEN_SETTINGS:
        lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
        settings_refresh(); // sync sliders to values changed via the buttons
        break;
    case SCREEN_APPROVAL:
        lv_obj_clear_flag(approval_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }

    if (logo_img)
        lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);

    current_screen = screen;
    apply_battery_visibility();
}

screen_t ui_get_current_screen(void)
{
    return current_screen;
}

// Carousel navigation. Virtual slot order: [Usage, Clock] + one slot per pending
// approval (s_approval_n). The approval slots map to SCREEN_APPROVAL with
// s_approval_view_idx selecting which one. Wraps around. Settings is not in the
// carousel — it is toggled by tapping the logo (logo_click_cb).
void ui_swipe(int dir)
{
    if (dir == 0)
        return;
    if (current_screen == SCREEN_SETTINGS)
        return; // logo-toggled, not swipeable

    int total = 2 + s_approval_n;      // Usage, Clock, then each approval
    int cur;
    if (current_screen == SCREEN_USAGE)
        cur = 0;
    else if (current_screen == SCREEN_CLOCK)
        cur = 1;
    else // SCREEN_APPROVAL
        cur = 2 + (s_approval_n ? s_approval_view_idx : 0);

    int next = ((cur + dir) % total + total) % total;

    if (next == 0)
        ui_show_screen(SCREEN_USAGE);
    else if (next == 1)
        ui_show_screen(SCREEN_CLOCK);
    else
    {
        s_approval_view_idx = (uint8_t)(next - 2);
        approval_refresh_labels();
        ui_show_screen(SCREEN_APPROVAL);
    }
}

void ui_update_ble_status(ble_state_t state, const char *name, const char *mac)
{
    (void)name;
    (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected)
        connected_at_ms = lv_tick_get();

    // On link loss the daemon can no longer drive the queue; drop any pending
    // approval view so we don't sit on a dead prompt. The host-side hooks stay
    // blocked and the approval reappears when the daemon reconnects and re-pushes.
    if (!s_ble_connected && was_connected)
    {
        s_approval_count = 0;
        s_approval_n = 0;
        s_approval_view_idx = 0;
        apply_claude_state(); // leaves SCREEN_APPROVAL if we were on it
    }

    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging)
{
    int idx;
    if (charging)
    {
        idx = 4;
    }
    else if (percent < 0)
    {
        idx = 0;
    }
    else if (percent <= 10)
    {
        idx = 0;
    }
    else if (percent <= 35)
    {
        idx = 1;
    }
    else if (percent <= 75)
    {
        idx = 2;
    }
    else
    {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}

void ui_update_volume(uint8_t idx)
{
    if (!volume_img || idx > 3)
        return;
    lv_image_set_src(volume_img, &volume_dscs[idx]);
}
