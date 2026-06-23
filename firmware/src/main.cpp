#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "ota.h"
#include "clock.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "volume.h"
#include "sleeptimeout.h"
#include "soundtheme.h"
#include "battery_estimate.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/audio_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

// Set while send_screenshot() is force-refreshing the screen. PSRAM-free
// boards can't hold a full framebuffer for lv_snapshot, so they capture by
// streaming each partial-render strip out the serial port as it flushes.
static volatile bool g_shot_active = false;

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    if (g_shot_active) {
        // A full-screen invalidate is split into contiguous, full-width bands
        // top-to-bottom; concatenating their px_maps reproduces the frame in
        // row-major RGB565 order — exactly what the host script expects.
        Serial.write((const uint8_t*)px_map, (size_t)w * h * 2);
        Serial.flush();
    }
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    strlcpy(out->error_msg, doc["em"] | "", sizeof(out->error_msg));
    out->host_epoch = doc["t"] | 0u;

    // Live Claude Code state (omitted on older daemons -> defaults to idle/no queue).
    out->claude_state = (claude_state_t)(int)(doc["cs"] | 0);
    out->approval_count = doc["aq"] | 0;

    // Bounded approval queue. Each entry: {tn: tool, td: detail, sn: session}.
    out->approval_n = 0;
    JsonArrayConst q = doc["q"].as<JsonArrayConst>();
    for (JsonObjectConst item : q) {
        if (out->approval_n >= MAX_APPROVALS)
            break;
        Approval &a = out->approvals[out->approval_n++];
        strlcpy(a.tool, item["tn"] | "", sizeof(a.tool));
        strlcpy(a.detail, item["td"] | "", sizeof(a.detail));
        strlcpy(a.session, item["sn"] | "", sizeof(a.session));
    }

    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB), so lv_snapshot is out. Instead force a
    // full redraw and stream each partial-render strip out the serial port as
    // my_flush_cb sees it — no full-frame buffer needed, just the existing
    // BUF_LINES strip already in SRAM.
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t buf_size = w * h * 2;
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    g_shot_active = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);          // renders + flushes every strip synchronously
    g_shot_active = false;
    Serial.println();
    Serial.println("SCREENSHOT_END");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle
    sleeptimeout_init(); // load the user's saved auto-sleep delay and apply to idle
    battery_estimate_init(); // load persisted per-1% rates for the runtime estimate

    power_hal_init();
    imu_hal_init();
    touch_hal_init();
    audio_hal_init();
    clock_init();       // seed wall-clock from the RTC if the board has one

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    // A null buffer would feed lv_display_set_buffers() garbage and crash on the
    // first render — indistinguishable from a hang. Most likely on the C6 (no
    // PSRAM: both buffers come from internal SRAM). Fail loud instead.
    if (!buf1 || !buf2) {
        Serial.printf("FATAL: LVGL buffer alloc failed (%d x %d x 2 each)\n", W, BUF_LINES);
        Serial.flush();
        for (;;) delay(1000);
    }

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    volume_init();   // load saved volume, apply to audio HAL, set the top-bar icon
    soundtheme_init(); // load saved sound theme, apply to audio HAL
    ui_show_screen(SCREEN_USAGE);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture on the BOOT (primary) button: hold ~3s → "release to
// pair" → release clears all BLE bonds, re-advertises, and opens a soft,
// time-limited pairing window. Pairing moved off the PWR button, so there is no
// power-off hold to collide with — hence no disarm stage (the old PWR gesture
// disarmed at 6s only to dodge the 8s AXP shutdown). Releasing before 3s simply
// cancels. The window is UI-only: the radio stays connectable throughout so a
// bonded host can always reconnect; it just bounds the "pairing" state the
// screen shows and auto-reverts after PAIR_WINDOW_MS (or on connect).
#define PAIR_HOLD_MS    3000     // hold this long before release-to-pair arms
#define PAIR_WINDOW_MS  120000   // soft open window after a release-to-pair
enum pair_state_t { PAIR_IDLE, PAIR_HELD, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_held_since   = 0;
static uint32_t     pair_window_until = 0;

static void pair_tick(void) {
    // Soft pairing-window expiry: revert the UI once it elapses or a host
    // connects. The BLE link itself is never closed here.
    if (pair_window_until &&
        (millis() >= pair_window_until || ble_get_state() == BLE_STATE_CONNECTED)) {
        pair_window_until = 0;
        ui_set_pair_hint(PAIR_HINT_IDLE);
    }

    // Only run the gesture while awake — a press that wakes the panel (consumed
    // in the button block) must not also start a pairing hold.
    if (idle_is_asleep()) { pair_state = PAIR_IDLE; return; }

    bool held = input_hal_is_held(INPUT_BTN_PRIMARY);

    if (pair_state == PAIR_IDLE) {
        if (held) { pair_state = PAIR_HELD; pair_held_since = millis(); }
        return;
    }
    if (!held) {  // released
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released — clearing bonds, advertising");
            ble_clear_bonds();
            pair_window_until = millis() + PAIR_WINDOW_MS;
            ui_set_pair_hint(PAIR_HINT_OPEN);
        }
        pair_state = PAIR_IDLE;
        return;
    }
    if (pair_state == PAIR_HELD && millis() - pair_held_since >= PAIR_HOLD_MS) {
        pair_state = PAIR_ARMED;
        ui_set_pair_hint(PAIR_HINT_ARMED);
        Serial.println("Pair: armed — release to pair");
    }
}

// Drive the OTA progress overlay and finalize a transfer. Finalization (flash
// commit + reboot) runs here, not in the BLE write callback, so it never blocks
// the BLE host task. The panel is kept awake while a transfer is in flight so
// the user can watch progress.
static void ota_tick(void) {
    static bool overlay_shown = false;
    static int  last_pct = -1;

    if (ota_active()) {
        ota_drain();              // commit bytes the BLE task buffered to flash
        idle_note_activity();     // don't sleep mid-update
        if (!overlay_shown) {
            idle_wake();
            ui_ota_show(true);
            overlay_shown = true;
            last_pct = -1;
        }
        int pct = ota_progress_pct();
        if (pct != last_pct) {
            last_pct = pct;
            ui_ota_set_pct(pct);
        }
    }

    if (ble_ota_finish_requested()) {
        if (ota_end()) {          // flushes the buffer, verifies, commits
            ui_ota_set_pct(100);
            ble_ota_notify_done();
            lv_timer_handler();   // flush the 100% frame before we go down
            delay(300);           // let the notify PDU + paint leave
            ESP.restart();        // boots the freshly committed image
        } else {
            ble_ota_notify_error(ota_last_error());
            ui_ota_show(false);
            overlay_shown = false;
        }
        return;
    }

    if (!ota_active() && overlay_shown) {
        // Aborted / disconnected mid-transfer — drop the overlay.
        ui_ota_show(false);
        overlay_shown = false;
    }
}

void loop() {
    idle_tick();

    // Battery saver: once the panel is asleep AND we're on battery (no USB),
    // the device only needs to listen for BLE writes and button presses. Skip
    // all the rendering/animation/sensor ticks that have nothing to do with a
    // dark screen, and slow the loop from 200Hz to 10Hz so the CPU spends most
    // of its time in vTaskDelay (and, with esp_pm, light-sleep). On USB or
    // while awake, run the full-rate loop as before.
    const bool battery_sleep = idle_is_asleep() && !power_hal_is_vbus_in();

    if (!battery_sleep) {
        lv_timer_handler();
        ui_tick_anim();
        audio_hal_tick();
        imu_hal_tick();
        // Rotation transition (blank + ramp) would fight the idle fade — skip
        // ticks while the panel is dark. A rotation that happens during sleep
        // is detected by the next tick after wake and ramped in then.
        if (!idle_is_asleep()) display_hal_tick();
    }

    ble_tick();
    ota_tick();
    power_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY (BOOT)   → hold ~3s + release: pairing (handled in pair_tick)
    //   SECONDARY (KEY)  → cycle to the next screen (only if the board has one)
    //   PWR              → short press toggles screen sleep/wake; power off/on
    //                      is AXP hardware (hold / press), not handled here
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press() (which also does the idle-timer activity
    // bookkeeping); the normal action fires from the second press. KEY is a tap
    // (fires on the press edge only). Brightness/volume now live in Settings.
    {
        // Drain the primary edge so a wake-press is consumed here and does not
        // also start a pairing hold (pair_tick is gated on !idle_is_asleep()).
        static bool primary_was = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now && !primary_was) (void)idle_consume_wake_press();
        primary_was = primary_now;

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now && !idle_consume_wake_press()) ui_swipe(+1);
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            // PWR toggles the screen: wakes if asleep (consume_wake_press
            // returns true), otherwise puts it to sleep.
            if (!idle_consume_wake_press()) idle_sleep_now();
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        battery_estimate_update(pct, charging); // learn the per-1% rate
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            if (usage.host_epoch) clock_sync_epoch(usage.host_epoch);
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    // 10Hz when sleeping on battery (enough for BLE/button latency), 200Hz when
    // awake for smooth UI. delay() maps to vTaskDelay, yielding to idle/light-sleep.
    delay(battery_sleep ? 100 : 5);
}
