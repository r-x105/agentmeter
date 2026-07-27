#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <WiFi.h>   // status overlay shows the config page's IP

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "provisioning.h"
#include "poller.h"
#include "providers/provider.h"
#include "providers/secrets.h"
#include "captive_portal.h"
#include "config_server.h"
#include "brightness.h"
#include "settings.h"
#include "board_check.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// Legacy-UI shim: project the primary provider's slot onto the old UsageData
// until the multi-provider tileview replaces ui.cpp's single-provider layout.
static void slot_to_usage(const ProviderSlot* s, UsageData* u) {
    u->session_pct        = s->primary.present   ? s->primary.value   : 0;
    u->session_reset_mins = s->primary.reset_mins;
    u->weekly_pct         = s->secondary.present ? s->secondary.value : 0;
    u->weekly_reset_mins  = s->secondary.reset_mins;
    strlcpy(u->status, s->status, sizeof(u->status));
    u->chime      = false;
    u->enterprise = false;
    u->ok = u->valid = s->valid;
}

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
//
// 120 lines (4 strips per full frame) rather than 40: measured on the 2.16,
// going from 40 to 120 cut LVGL's render time from 36 ms to 26 ms per frame,
// since each flush carries its own setup cost. PSRAM cost is 2 × 480×120×2 =
// 230 KB, which is nothing against 7.7 MB free.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 120
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

// Split of a frame's cost: time spent getting pixels onto the panel, vs. time
// LVGL spent rendering them. Read by the `bench` serial command — the two are
// worth telling apart, since only one of them responds to the bus clock.
// Both are only touched from the LVGL flush callback and the serial command
// handler, which run on the same task — no volatile needed.
static uint32_t g_push_us = 0;
static uint32_t g_push_calls = 0;

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint32_t t0 = micros();
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    g_push_us += micros() - t0;
    g_push_calls++;
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
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);

    // touch_hal_read returns true panel coordinates; the display is rotated in
    // software by the IMU quadrant (board display.cpp rotate_strip), so map the
    // touch by the INVERSE rotation to line up with the on-screen layout. This
    // is the inverse of rotate_strip's LVGL->panel map. No-op at quadrant 0 and
    // on boards without rotation.
    if (pressed) {
        uint8_t q = imu_hal_rotation_quadrant();
        if (q) {
            const uint16_t S = board_caps().width - 1;   // square panel
            uint16_t px = x, py = y;
            switch (q) {
                case 1: x = py;     y = S - px; break;
                case 2: x = S - px; y = S - py; break;
                case 3: x = S - py; y = px;     break;
            }
        }
    }
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



// ---- Serial command buffer ----
#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
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
            if (cmd_pos > 0) {
                if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
                else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
                else if (strcmp(cmd_buf, "bench") == 0) {   // QA: full-frame redraw cost
                    g_push_us = 0; g_push_calls = 0;
                    const int N = 20;
                    uint32_t t0 = millis();
                    for (int i = 0; i < N; i++) {
                        lv_obj_invalidate(lv_screen_active());
                        lv_refr_now(NULL);
                    }
                    uint32_t dt = millis() - t0;
                    Serial.printf("bench: %d frames in %lums = %lu ms/frame (%lu fps); "
                        "push %lu ms/frame over %lu strips = %lu%% of frame\n",
                        N, (unsigned long)dt, (unsigned long)(dt / N),
                        (unsigned long)(dt ? N * 1000UL / dt : 0),
                        (unsigned long)(g_push_us / 1000UL / N),
                        (unsigned long)(g_push_calls / N),
                        (unsigned long)(dt ? (g_push_us / 10UL) / dt : 0));
                }
                else if (strcmp(cmd_buf, "lvmem") == 0) {   // QA: LVGL pool headroom
                    lv_mem_monitor_t m;
                    lv_mem_monitor(&m);
                    Serial.printf("lvmem: free=%u frag=%u%% internal=%u\n",
                        (unsigned)m.free_size, (unsigned)m.frag_pct,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                }
                else if (strncmp(cmd_buf, "tile ", 5) == 0) {   // QA: "tile <col> [page]"
                    int col = 0, page = 0;
                    sscanf(cmd_buf + 5, "%d %d", &col, &page);
                    ui_show_tile(col, page);
                }
                else if (strncmp(cmd_buf, "screen ", 7) == 0) {   // QA: jump screens
                    const char* s = cmd_buf + 7;
                    if      (strcmp(s, "usage") == 0)    ui_show_screen(SCREEN_USAGE);
                    else if (strcmp(s, "settings") == 0) ui_show_screen(SCREEN_SETTINGS);
                    else if (strcmp(s, "wifi") == 0)     ui_show_screen(SCREEN_WIFI);
                    else if (strcmp(s, "splash") == 0)   ui_show_screen(SCREEN_SPLASH);
                }
                else if (strncmp(cmd_buf, "cred ", 5) == 0) {
                    // cred <provider-id> <suffix> <value>  e.g. cred openrouter tk sk-or-…
                    char id[16], sfx[4], val[224];
                    if (sscanf(cmd_buf + 5, "%15s %3s %223s", id, sfx, val) == 3 &&
                        provider_index_by_id(id) >= 0) {
                        secrets_set(id, sfx, val);
                        provider_refresh_config(provider_index_by_id(id));
                        ui_rebuild_provider_tiles();
                    } else {
                        Serial.println("cred: usage: cred <anthropic|openrouter> <tk|en> <value>");
                    }
                }
                else {
                    provisioning_handle_cmd(cmd_buf);
                    ui_update_wifi_creds(captive_portal_is_active());
                }
            }
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
    provisioning_init();
    secrets_init();
    provider_registry_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle
    settings_init();    // load persisted UI prefs (status-text toggle) before ui_init()

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

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
    poller_init();
    captive_portal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_update_wifi_creds(captive_portal_is_active());
    if (captive_portal_is_active()) {
        ui_set_nav_locked(true);
        ui_show_screen(SCREEN_WIFI);
    } else {
        ui_show_screen(SCREEN_SPLASH);
    }

    Serial.printf("Dashboard ready (%s, %dx%d)\n", board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    board_check_tick();   // re-warns on a wrong-board flash; no-op otherwise
    idle_tick();
    lv_timer_handler();
    secrets_flush();   // persist queued credential writes (see secrets.h)
    poller_tick();
    captive_portal_tick();
    config_server_tick();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. Otherwise cycle screens
                // (Usage -> Settings -> Wi-Fi). Brightness now lives in Settings.
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          ui_cycle_screen();
            }
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
        ui_update_battery(pct, charging);
    }

    {   // bottom-left ring tracks the primary provider's next poll
        int p = provider_primary_index();
        ui_set_refresh_progress(p >= 0 ? poller_slot_progress(p) : 0);
    }

    // Refresh the Wi-Fi screen when connectivity flips — the Config row's
    // IP address only exists once the STA link is up.
    {
        static bool last_conn = false;
        bool conn = poller_is_connected();
        if (conn != last_conn) {
            last_conn = conn;
            ui_update_wifi_creds(captive_portal_is_active());
        }
    }

    // Start hotspot: manual button on Wi-Fi screen, or 3 successive connect failures.
    if (!captive_portal_is_active()) {
        bool manual = ui_hotspot_requested();
        bool auto_fail = (poller_wifi_fail_count() >= 3);
        if (manual || auto_fail) {
            if (auto_fail) Serial.println("wifi: 3 failures, switching to hotspot");
            poller_stop();
            captive_portal_start();
            ui_update_wifi_creds(true);
            ui_set_nav_locked(true);
            ui_show_screen(SCREEN_WIFI);
        }
    }

    if (ui_reboot_requested()) ESP.restart();   // Wi-Fi "Back" while hotspot active

    check_serial_cmd();

    if (provider_take_new_data()) {
        // Chime / splash-mood / rate logic binds to the primary provider;
        // other slots are display-only (rendered by the tileview).
        ProviderSlot prim = {};
        int p = provider_primary_index();
        if (p >= 0) provider_snapshot(p, &prim);
        if (prim.valid) slot_to_usage(&prim, &usage);
        int g_before = usage_rate_group();
        bool session_reset = usage_rate_sample(usage.session_pct);
        int g_after = usage_rate_group();
        // 5-hour session limit refilled → chime (no-op on boards without a
        // buzzer). Gated on usage.chime; wifi_poller leaves it false today, so
        // this stays silent unless a future payload sets it.
        if (session_reset && usage.chime) {
            Serial.println("session reset detected — chime");
            sound_hal_play_reset();
        }
        if (g_after != g_before) {
            Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                g_before, g_after, usage.session_pct);
            if (splash_is_active()) splash_pick_for_current_rate();
        }
        ui_update_providers();
    }

    // Status overlay — Wi-Fi-level state first, then the primary provider's
    // slot state. Updated whenever either changes (or data validity flips).
    {
        static uint32_t last_key  = 0xFFFFFFFF;
        poller_wifi_status_t ws = poller_wifi_status();
        int p = provider_primary_index();
        ProviderSlot prim = {};
        if (p >= 0) provider_snapshot(p, &prim);
        uint32_t key = ((uint32_t)ws << 16) | ((uint32_t)prim.state << 8) |
                       ((p < 0) << 1) | (usage.valid ? 1 : 0);
        if (key != last_key) {
            last_key = key;
            char msg[80] = {};
            ui_status_level_t lvl = UI_STATUS_INFO;
            if (ws == POLLER_CONNECTING) {
                strlcpy(msg, "Connecting to Wi-Fi\xE2\x80\xA6", sizeof(msg));
            } else if (ws == POLLER_WIFI_FAIL) {
                strlcpy(msg, "Wi-Fi error \xE2\x80\x94 check credentials", sizeof(msg));
                lvl = UI_STATUS_ERROR;
            } else if (p < 0) {
                // Connected but no provider is configured+enabled yet — point
                // at the config page by IP (mDNS doesn't resolve everywhere).
                snprintf(msg, sizeof(msg), "Setup: http://%s",
                         WiFi.localIP().toString().c_str());
                lvl = UI_STATUS_WARN;
            } else switch (prim.state) {
                case PROV_AUTH_NEEDED:
                    snprintf(msg, sizeof(msg), "%s: re-auth needed", prim.def->name);
                    lvl = UI_STATUS_ERROR; break;
                case PROV_LIMITED:
                    snprintf(msg, sizeof(msg), "%s: usage limit reached", prim.def->name);
                    lvl = UI_STATUS_ERROR; break;
                case PROV_DOWN:
                    snprintf(msg, sizeof(msg), "%s API down", prim.def->name);
                    lvl = UI_STATUS_ERROR; break;
                case PROV_ERROR:
                    if (prim.last_http_code < 0)
                        snprintf(msg, sizeof(msg), "%s unreachable", prim.def->name);
                    else
                        snprintf(msg, sizeof(msg), "%s error %d", prim.def->name,
                                 prim.last_http_code);
                    lvl = UI_STATUS_ERROR; break;
                default: break;  // INIT, OK — msg stays ""
            }
            // Keep overlay until first data arrives even if states look fine.
            if (msg[0] == '\0' && !usage.valid) {
                strlcpy(msg, "Connecting\xE2\x80\xA6", sizeof(msg));
            }
            ui_set_status(lvl, msg[0] ? msg : nullptr);
        }
    }

    delay(1);
}
