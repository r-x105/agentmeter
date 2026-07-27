#include "ui.h"
#include "ui_common.h"
#include "ui_overview.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "provisioning.h"
#include "brightness.h"
#include "settings.h"
#include "providers/provider.h"
#include "config_server.h"   // Wi-Fi screen shows the device code
#include <WiFi.h>            // …and the LAN IP of the config page

// The layout table, the color aliases, the widget factories and the
// provider→text helpers live in ui_common.{h,cpp}; the overview tile lives in
// ui_overview.{h,cpp}. This file owns the tileview shell, the per-provider
// cards, and the splash / settings / Wi-Fi screens.

// ---- Usage screen widgets (tileview: overview + one card per provider) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;         // big HH:MM (or "Usage")
static lv_obj_t* lbl_title_small;   // small :SS / AM-PM suffix on the clock
static lv_obj_t* arc_refresh;       // bottom-left ring: progress to next poll (primary)
static int       s_refresh_pct = -1;
static int      clock_last_sec = -1;   // last rendered second; avoids redundant title redraws
static lv_obj_t* usage_group;   // the provider tileview — shown when data is fresh
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle
static lv_obj_t* lbl_status;    // Wi-Fi status overlay (shown in place of lbl_anim)

// One metric panel (big value + pill + bar + reset line) inside a card.
struct MetricRow {
    lv_obj_t *panel, *val, *pill, *bar, *reset;
    lv_obj_t *rule;   // hairline below the block; hidden when nothing follows
};
// A provider's swipe tile: header row (color dot + name + age) + two metrics.
struct ProviderCard {
    int       slot;         // registry index
    lv_obj_t* tile;
    lv_obj_t* head_dot;
    lv_obj_t* head_name;
    lv_obj_t* head_age;     // "12m ago" staleness, right-aligned
    MetricRow m1, m2;
};
static lv_obj_t*    tiles_view = nullptr;   // lv_tileview (== usage_group)
static ProviderCard cards[MAX_PROVIDERS];
static int          n_cards = 0;
static lv_obj_t*    dots[MAX_PROVIDERS + 1];   // horizontal: one per column
static int          n_dots = 0;

// ---- Wi-Fi screen widgets ----
static lv_obj_t* wifi_container;
static lv_obj_t* lbl_wifi_ssid_val;
static lv_obj_t* lbl_wifi_pass_val;
static lv_obj_t* lbl_wifi_token_val;   // "Config" row: the config page's LAN URL
static lv_obj_t* lbl_wifi_code_val;    // rotating device code gating /save
static lv_obj_t* lbl_wifi_note;

// ---- Settings screen widgets ----
static lv_obj_t* settings_container;
static lv_obj_t* settings_confirm;      // "Open Wi-Fi setup?" confirm overlay
static lv_obj_t* settings_clock;        // clock config overlay
static lv_obj_t* bar_brightness;        // shows the current brightness level
static lv_obj_t* lbl_status_toggle;     // "On" / "Off" for the status line
static lv_obj_t* lbl_clock_tile;        // "On"/"Off" on the Clock tile
static lv_obj_t* lbl_clock_enable;      // "On"/"Off" in the clock overlay
static lv_obj_t* lbl_clock_offset;      // "UTC+5:30" in the clock overlay
static lv_obj_t* lbl_clock_fmt;         // "24-hour"/"12-hour" in the clock overlay
static lv_obj_t* lbl_clock_secs;        // "On"/"Off" seconds toggle
static bool      s_show_status_text = true;   // mirrors settings_status_text()

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void wifi_hotspot_click_cb(lv_event_t* e);
static void wifi_back_cb(lv_event_t* e);

static bool s_hotspot_requested = false;
static bool s_nav_locked        = false;
static bool s_portal_active     = false;   // mirrors captive_portal_is_active()
static bool s_reboot_requested  = false;   // Wi-Fi "Back" while the hotspot is up

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
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

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

// One metric block on a provider card, in the same language as an overview hero
// row: a dim caps eyebrow naming the window, the measurement, a meter in the
// provider's color, and the reset line. No card fill, no pill — a hairline rule
// separates it from the block below.
static void make_metric_row(lv_obj_t* parent, int y, MetricRow* m, bool rule) {
    const int block_h = L.card_metric_h;

    m->panel = lv_obj_create(parent);
    lv_obj_set_pos(m->panel, L.margin, y);
    lv_obj_set_size(m->panel, L.content_w, block_h);
    lv_obj_set_style_bg_opa(m->panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m->panel, 0, 0);
    lv_obj_set_style_radius(m->panel, 0, 0);
    lv_obj_set_style_pad_all(m->panel, 0, 0);
    lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m->panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    m->rule = nullptr;
    if (rule) {
        m->rule = lv_obj_create(m->panel);
        lv_obj_set_size(m->rule, L.content_w, 1);
        lv_obj_set_pos(m->rule, 0, block_h - 1);
        lv_obj_set_style_bg_color(m->rule, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(m->rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(m->rule, 0, 0);
        lv_obj_set_style_radius(m->rule, 0, 0);
        lv_obj_clear_flag(m->rule, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(m->rule, LV_OBJ_FLAG_CLICKABLE);
    }

    // The stack is centered in the block so the hairline rule always has air
    // around it rather than sitting on the reset line.
    const int eyebrow_h = lv_font_get_line_height(L.card_eyebrow_font);
    const int val_h     = lv_font_get_line_height(L.card_val_font);
    const int detail_h  = lv_font_get_line_height(L.card_detail_font);
    const int bar_h     = 8;
    const int stack_h   = eyebrow_h + 4 + val_h + 5 + bar_h + 6 + detail_h;
    const int start_y   = (block_h > stack_h) ? (block_h - stack_h) / 2 : 0;

    // The window's name is the eyebrow here — the provider is already named in
    // the card header above.
    m->pill = lv_label_create(m->panel);
    lv_label_set_text(m->pill, "");
    lv_obj_set_style_text_font(m->pill, L.card_eyebrow_font, 0);
    lv_obj_set_style_text_color(m->pill, COL_DIM, 0);
    lv_obj_set_style_text_letter_space(m->pill, 2, 0);
    lv_obj_set_pos(m->pill, 0, start_y);

    m->val = lv_label_create(m->panel);
    lv_label_set_text(m->val, "---");
    lv_obj_set_style_text_font(m->val, L.card_val_font, 0);
    lv_obj_set_style_text_color(m->val, COL_TEXT, 0);
    lv_obj_set_pos(m->val, 0, start_y + eyebrow_h + 4);

    const int bar_y = start_y + eyebrow_h + 4 + val_h + 5;
    m->bar = make_bar(m->panel, 0, bar_y, L.content_w, bar_h);
    lv_obj_set_style_radius(m->bar, bar_h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(m->bar, bar_h / 2, LV_PART_INDICATOR);

    m->reset = lv_label_create(m->panel);
    lv_label_set_text(m->reset, "---");
    lv_obj_set_style_text_font(m->reset, L.card_detail_font, 0);
    lv_obj_set_style_text_color(m->reset, COL_DIM, 0);
    lv_obj_set_pos(m->reset, 0, bar_y + bar_h + 6);
}

// Highlight the dot of the active column (index 0 = overview).
static void update_dots(int active) {
    for (int i = 0; i < n_dots; i++) {
        lv_obj_set_style_bg_color(dots[i],
            i == active ? COL_TEXT : COL_BAR_BG, 0);
    }
}

void ui_show_tile(int col, int page) {
    if (!tiles_view || col < 0 || col > n_cards) return;
    if (col != 0) page = 0;                       // cards all live on row 0
    if (page < 0 || page >= overview_page_count()) page = 0;
    lv_tileview_set_tile_by_index(tiles_view, col, page, LV_ANIM_OFF);
    update_dots(col);
    overview_set_active(col, page);
}

static void tiles_scroll_cb(lv_event_t* e) {
    (void)e;
    if (!tiles_view) return;
    lv_obj_t* act = lv_tileview_get_tile_active(tiles_view);
    if (!act) return;
    const int col  = lv_obj_get_x(act) / L.scr_w;
    const int page = L.tiles_h ? lv_obj_get_y(act) / L.tiles_h : 0;
    update_dots(col);
    overview_set_active(col, page);
}

// The tileview is recreated rather than emptied on every rebuild. lv_tileview
// keeps an internal pointer to its active tile, and lv_obj_clean() frees that
// tile without clearing the pointer — the next scroll or set_tile then walks
// freed memory and takes the device down. Recreating the widget is the only way
// to be sure no stale pointer survives.
static void create_tiles_view(void) {
    tiles_view = lv_tileview_create(usage_container);
    usage_group = tiles_view;   // update_view_state toggles it against idle_group
    lv_obj_set_size(tiles_view, L.scr_w, L.tiles_h);
    lv_obj_set_pos(tiles_view, 0, L.content_y);
    lv_obj_set_style_bg_opa(tiles_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tiles_view, 0, 0);
    lv_obj_set_style_pad_all(tiles_view, 0, 0);
    lv_obj_set_scrollbar_mode(tiles_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(tiles_view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(tiles_view, tiles_scroll_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Build the overview pages (column 0) and one card tile per enabled provider
// (columns 1..N). Called at init and whenever the enabled set changes.
void ui_rebuild_provider_tiles(void) {
    if (!usage_container) return;

    bool was_hidden = tiles_view && lv_obj_has_flag(tiles_view, LV_OBJ_FLAG_HIDDEN);
    if (tiles_view) lv_obj_delete(tiles_view);
    create_tiles_view();
    if (was_hidden) lv_obj_add_flag(tiles_view, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < n_dots; i++) lv_obj_delete(dots[i]);   // parented to usage_container
    n_dots = 0;

    int slots[MAX_PROVIDERS];
    n_cards = enabled_slots(slots, MAX_PROVIDERS);

    // ---- Column 0: the overview, paginated vertically ----
    overview_build(tiles_view, usage_container);

    // ---- Columns 1..N: one card per enabled provider ----
    for (int c = 0; c < n_cards; c++) {
        ProviderCard* card = &cards[c];
        card->slot = slots[c];
        ProviderSlot s;
        provider_snapshot(card->slot, &s);

        card->tile = lv_tileview_add_tile(tiles_view, c + 1, 0,
            (lv_dir_t)(LV_DIR_LEFT | (c + 1 < n_cards ? LV_DIR_RIGHT : 0)));
        lv_obj_clear_flag(card->tile, LV_OBJ_FLAG_SCROLLABLE);

        // Header names the provider in the overview's own voice: a color dot
        // and a dim caps eyebrow, with the state string at the right.
        const int head_font_h = lv_font_get_line_height(L.card_head_font);

        card->head_dot = make_color_dot(card->tile, s.def->color, 10);
        lv_obj_set_pos(card->head_dot, L.margin, (head_font_h - 10) / 2);

        char caps[24];
        upcase(s.def->name, caps, sizeof(caps));
        card->head_name = lv_label_create(card->tile);
        lv_label_set_text(card->head_name, caps);
        lv_obj_set_style_text_font(card->head_name, L.card_head_font, 0);
        lv_obj_set_style_text_color(card->head_name, COL_DIM, 0);
        lv_obj_set_style_text_letter_space(card->head_name, 2, 0);
        lv_obj_set_pos(card->head_name, L.margin + 18, 0);

        card->head_age = lv_label_create(card->tile);
        lv_label_set_text(card->head_age, "");
        lv_obj_set_style_text_font(card->head_age, L.card_eyebrow_font, 0);
        lv_obj_set_style_text_color(card->head_age, COL_DIM, 0);
        lv_obj_align(card->head_age, LV_ALIGN_TOP_RIGHT, -L.margin, 2);

        make_metric_row(card->tile, L.card_head_h, &card->m1, true);
        make_metric_row(card->tile, L.card_head_h + L.card_metric_h, &card->m2, false);
    }

    // ---- Page dots (hidden when there's nothing to swipe to) ----
    n_dots = n_cards > 0 ? n_cards + 1 : 0;
    if (n_dots > 1) {
        const int dd = 10, gap = 10;
        int total = n_dots * dd + (n_dots - 1) * gap;
        for (int i = 0; i < n_dots; i++) {
            dots[i] = make_color_dot(usage_container, 0x000000, dd);
            lv_obj_set_pos(dots[i], (L.scr_w - total) / 2 + i * (dd + gap),
                           L.scr_h - 52);
        }
        update_dots(0);
    }

    ui_update_providers();
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Title is a flex row: big HH:MM (or "Usage") + a small :SS / AM-PM suffix,
    // bottom-aligned so the small text sits near the big text's baseline.
    lv_obj_t* title_row = lv_obj_create(usage_container);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 3, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 16, L.title_y);

    lbl_title = lv_label_create(title_row);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);

    lbl_title_small = lv_label_create(title_row);
    lv_label_set_text(lbl_title_small, "");
    lv_obj_set_style_text_font(lbl_title_small, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_title_small, COL_DIM, 0);
    lv_obj_set_style_pad_bottom(lbl_title_small, 8, 0);   // lift toward the baseline

    // Provider tileview (shown when data is fresh): tile 0 = overview, then
    // one card per enabled provider, swiped horizontally. It fills the zone
    // between the header and the page dots / status line.
    ui_rebuild_provider_tiles();   // creates the tileview and fills it

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    lbl_status = lv_label_create(usage_container);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_style_text_font(lbl_status, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_status, L.content_w);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);

    // Bottom-left ring: progress toward the next Anthropic poll (display-only).
    const int arc_d = 34;
    arc_refresh = lv_arc_create(usage_container);
    lv_obj_set_size(arc_refresh, arc_d, arc_d);
    lv_obj_set_pos(arc_refresh, L.margin, L.scr_h - L.margin - arc_d);
    lv_arc_set_rotation(arc_refresh, 270);       // fill starts at 12 o'clock
    lv_arc_set_bg_angles(arc_refresh, 0, 360);   // full ring background
    lv_arc_set_range(arc_refresh, 0, 100);
    lv_arc_set_value(arc_refresh, 0);
    lv_obj_clear_flag(arc_refresh, LV_OBJ_FLAG_CLICKABLE);   // not interactive
    lv_obj_set_style_bg_opa(arc_refresh, LV_OPA_TRANSP, LV_PART_KNOB);  // hide knob
    lv_obj_set_style_pad_all(arc_refresh, 0, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc_refresh, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_refresh, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_refresh, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_refresh, COL_ACCENT, LV_PART_INDICATOR);
}

// ======== Wi-Fi Screen ========

static void redact_password(const String& pass, char* buf, size_t len) {
    if (pass.length() == 0) { strlcpy(buf, "(none)", len); return; }
    if (pass.length() == 1) { strlcpy(buf, "*", len); return; }
    if (pass.length() == 2) { strlcpy(buf, "**", len); return; }
    snprintf(buf, len, "%c***%c", pass[0], pass[pass.length() - 1]);
}

static lv_obj_t* make_wifi_val_label(lv_obj_t* parent, int y) {
    int val_w = L.content_w - 32 - L.wifi_val_x;
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    // Fixed size + CLIP enforces single-line; text is pre-truncated in software.
    lv_obj_set_size(lbl, val_w, L.wifi_row_h);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, "(none)");
    lv_obj_set_pos(lbl, L.wifi_val_x, y);
    return lbl;
}

static void init_wifi_screen(lv_obj_t* scr) {
    wifi_container = lv_obj_create(scr);
    lv_obj_set_size(wifi_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(wifi_container, 0, 0);
    lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_container, 0, 0);
    lv_obj_set_style_pad_all(wifi_container, 0, 0);
    lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wifi_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_title = lv_label_create(wifi_container);
    lv_label_set_text(lbl_title, "Wi-Fi");
    lv_obj_set_style_text_font(lbl_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    lv_obj_t* p = make_panel(wifi_container, L.margin, L.content_y,
                             L.content_w, L.wifi_panel_h);

    static const char* const keys[] = { "SSID", "Pass", "Config", "Code" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t* k = lv_label_create(p);
        lv_label_set_text(k, keys[i]);
        lv_obj_set_style_text_font(k, L.bt_device_font, 0);
        lv_obj_set_style_text_color(k, COL_DIM, 0);
        lv_obj_set_pos(k, 0, i * L.wifi_row_h);
    }

    lbl_wifi_ssid_val  = make_wifi_val_label(p, 0);
    lbl_wifi_pass_val  = make_wifi_val_label(p, L.wifi_row_h);
    lbl_wifi_token_val = make_wifi_val_label(p, 2 * L.wifi_row_h);
    lbl_wifi_code_val  = make_wifi_val_label(p, 3 * L.wifi_row_h);
    lv_obj_set_style_text_color(lbl_wifi_code_val, COL_ACCENT, 0);
    lv_obj_set_style_text_letter_space(lbl_wifi_code_val, 3, 0);

    // "New" pill at the end of the Code row — tap to rotate the device code.
    // Physical presence at the panel is the authority in this model, so the
    // rotate control lives here, not on the web page. No EVENT_BUBBLE: the
    // tap must not fall through to the panel's splash-toggle click.
    lv_obj_t* rot = make_pill(p, "New");
    lv_obj_set_style_text_font(rot, L.bt_device_font, 0);
    lv_obj_set_style_text_color(rot, COL_TEXT, 0);
    lv_obj_align(rot, LV_ALIGN_TOP_RIGHT, 0, 3 * L.wifi_row_h - 6);
    lv_obj_add_flag(rot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rot, [](lv_event_t* e) {
        (void)e;
        config_server_rotate_code();   // refreshes the row via ui_update_wifi_creds
    }, LV_EVENT_CLICKED, NULL);

    int btn_y  = L.content_y + L.wifi_panel_h + 16;
    int back_y = btn_y + L.wifi_btn_h + 10;
    int note_y = back_y + L.wifi_btn_h + 2;   // tight: 4-row panel above pushes everything down

    lv_obj_t* btn_zone = lv_obj_create(wifi_container);
    lv_obj_set_pos(btn_zone, L.margin, btn_y);
    lv_obj_set_size(btn_zone, L.content_w, L.wifi_btn_h);
    lv_obj_set_style_bg_color(btn_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(btn_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_zone, 8, 0);
    lv_obj_set_style_border_width(btn_zone, 0, 0);
    lv_obj_set_flex_flow(btn_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_zone, wifi_hotspot_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_lbl = lv_label_create(btn_zone);
    lv_label_set_text(btn_lbl, "Start Hotspot");
    lv_obj_set_style_text_font(btn_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(btn_lbl, COL_AMBER, 0);

    // Back to Usage — always an escape from this screen (reboots out if the
    // hotspot is running, since there's no clean captive-portal teardown).
    lv_obj_t* back_zone = lv_obj_create(wifi_container);
    lv_obj_set_pos(back_zone, L.margin, back_y);
    lv_obj_set_size(back_zone, L.content_w, L.wifi_btn_h);
    lv_obj_set_style_bg_color(back_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(back_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_zone, 8, 0);
    lv_obj_set_style_border_width(back_zone, 0, 0);
    lv_obj_set_flex_flow(back_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(back_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(back_zone, wifi_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_lbl = lv_label_create(back_zone);
    lv_label_set_text(back_lbl, "Back to Usage");
    lv_obj_set_style_text_font(back_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(back_lbl, COL_TEXT, 0);

    lbl_wifi_note = lv_label_create(wifi_container);
    lv_obj_set_style_text_font(lbl_wifi_note, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_note, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_wifi_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_wifi_note, L.content_w);
    lv_label_set_long_mode(lbl_wifi_note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_wifi_note, "Configure via serial at 115200 baud");
    lv_obj_set_pos(lbl_wifi_note, L.margin, note_y);

    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Settings Screen ========

// Show/hide the whimsical bottom line per the user's setting. Kept hidden while
// a Wi-Fi status overlay (lbl_status) is up — that path owns lbl_anim then.
static void apply_status_text(void) {
    if (!lbl_anim || !lbl_status) return;
    bool overlay_up = !lv_obj_has_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    if (s_show_status_text && !overlay_up) lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
    else                                    lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
}

// Sync the settings widgets to the live values (called when the screen opens).
static void settings_refresh(void) {
    if (bar_brightness) {
        uint8_t n = brightness_count();
        int pct = (n > 1) ? (brightness_index() * 100) / (n - 1) : 100;
        lv_bar_set_value(bar_brightness, pct, LV_ANIM_OFF);
    }
    if (lbl_status_toggle) {
        lv_label_set_text(lbl_status_toggle, s_show_status_text ? "On" : "Off");
        lv_obj_set_style_text_color(lbl_status_toggle,
            s_show_status_text ? COL_GREEN : COL_DIM, 0);
    }
    bool ck = settings_clock_enabled();
    if (lbl_clock_tile) {
        lv_label_set_text(lbl_clock_tile, ck ? "On" : "Off");
        lv_obj_set_style_text_color(lbl_clock_tile, ck ? COL_GREEN : COL_DIM, 0);
    }
    if (lbl_clock_enable) {
        lv_label_set_text(lbl_clock_enable, ck ? "On" : "Off");
        lv_obj_set_style_text_color(lbl_clock_enable, ck ? COL_GREEN : COL_DIM, 0);
    }
    if (lbl_clock_offset) {
        int off = settings_clock_offset_min();
        int ao = off < 0 ? -off : off;
        char b[16];
        snprintf(b, sizeof(b), "UTC%c%d:%02d", off < 0 ? '-' : '+', ao / 60, ao % 60);
        lv_label_set_text(lbl_clock_offset, b);
    }
    if (lbl_clock_fmt) {
        // Show both options; the active one bright-green, the other dim.
        lv_label_set_text(lbl_clock_fmt, settings_clock_24h()
            ? "#788c5d 24h#  #b0aea5 12h#"
            : "#b0aea5 24h#  #788c5d 12h#");
    }
    if (lbl_clock_secs) {
        bool sc = settings_clock_seconds();
        lv_label_set_text(lbl_clock_secs, sc ? "On" : "Off");
        lv_obj_set_style_text_color(lbl_clock_secs, sc ? COL_GREEN : COL_DIM, 0);
    }
}

static void brightness_row_cb(lv_event_t* e) { (void)e; brightness_cycle(); settings_refresh(); }

static void status_toggle_cb(lv_event_t* e) {
    (void)e;
    s_show_status_text = !s_show_status_text;
    settings_set_status_text(s_show_status_text);
    apply_status_text();
    settings_refresh();
}

// Wi-Fi setup asks for confirmation first — it starts a hotspot and drops the
// Wi-Fi link, so an accidental tap shouldn't trigger it.
static void wifi_setup_cb(lv_event_t* e) {
    (void)e;
    if (settings_confirm) lv_obj_clear_flag(settings_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void confirm_yes_cb(lv_event_t* e) {
    (void)e;
    if (settings_confirm) lv_obj_add_flag(settings_confirm, LV_OBJ_FLAG_HIDDEN);
    s_hotspot_requested = true;   // loop() -> captive portal + Wi-Fi screen
}

static void confirm_cancel_cb(lv_event_t* e) {
    (void)e;
    if (settings_confirm) lv_obj_add_flag(settings_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void back_click_cb(lv_event_t* e) { (void)e; ui_show_screen(SCREEN_USAGE); }

static void clock_tile_cb(lv_event_t* e) {
    (void)e;
    if (settings_clock) lv_obj_clear_flag(settings_clock, LV_OBJ_FLAG_HIDDEN);
}
static void clock_back_cb(lv_event_t* e) {
    (void)e;
    if (settings_clock) lv_obj_add_flag(settings_clock, LV_OBJ_FLAG_HIDDEN);
}
static void clock_enable_cb(lv_event_t* e) {
    (void)e;
    settings_set_clock_enabled(!settings_clock_enabled());
    clock_last_sec = -1;   // force the title to re-render (or revert to "Usage")
    settings_refresh();
}
static void clock_off_dec_cb(lv_event_t* e) { (void)e; settings_adjust_clock_offset(-30); clock_last_sec = -1; settings_refresh(); }
static void clock_off_inc_cb(lv_event_t* e) { (void)e; settings_adjust_clock_offset(+30); clock_last_sec = -1; settings_refresh(); }
static void clock_fmt_cb(lv_event_t* e) {
    (void)e;
    settings_set_clock_24h(!settings_clock_24h());
    clock_last_sec = -1;
    settings_refresh();
}
static void clock_secs_cb(lv_event_t* e) {
    (void)e;
    settings_set_clock_seconds(!settings_clock_seconds());
    clock_last_sec = -1;
    settings_refresh();
}

// Full-width tappable settings row — the WHOLE row is the tap target, so an
// imprecise touch (this panel calibrates to ~30px) can't miss it. A left label
// plus, for value rows, a right-aligned widget added by the caller. `center`
// makes a single centered-label button (Pair / Re-provision / Back).
static lv_obj_t* make_settings_row(lv_obj_t* parent, int y, int h, const char* left,
                                   lv_color_t left_col, bool center, lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_pos(row, L.margin, y);
    lv_obj_set_size(row, L.content_w, h);
    lv_obj_set_style_bg_color(row, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 18, 0);
    lv_obj_set_style_pad_right(row, 18, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, left);
    lv_obj_set_style_text_font(l, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l, left_col, 0);
    return row;
}

// A big square-ish settings tile (used in a 2x2 grid). The whole tile is the
// tap target. Title label is child 0; a value widget (bar / On-Off) can be
// appended by the caller and will stack centered below the title.
static lv_obj_t* make_tile(lv_obj_t* parent, int x, int y, int w, int h,
                           const char* title, lv_color_t col, lv_event_cb_t cb) {
    lv_obj_t* t = lv_obj_create(parent);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_bg_color(t, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_set_style_pad_row(t, 10, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(t);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_font(l, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return t;
}

// Small square −/+ button for the clock UTC-offset stepper.
static lv_obj_t* make_step_btn(lv_obj_t* parent, int h, const char* text, lv_event_cb_t cb) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_size(b, 56, h - 16);
    lv_obj_set_style_bg_color(b, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);
    return b;
}

static void init_settings_screen(lv_obj_t* scr) {
    settings_container = lv_obj_create(scr);
    lv_obj_set_size(settings_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_container, 0, 0);
    lv_obj_set_style_bg_opa(settings_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_set_style_pad_all(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);
    // No tap-to-splash here — every tap belongs to a control row, and "Back to
    // Usage" is an explicit button, so a stray tap can't bounce you away.

    lv_obj_t* title = lv_label_create(settings_container);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // 2x2 grid of big tiles + a full-width Back bar below. Large tap targets.
    int gap    = 16;
    int back_h = L.set_row_h;   // Back bar uses make_settings_row (set_row_h tall)
    int tw  = (L.content_w - gap) / 2;
    int top = L.content_y;
    int grid_bot = L.scr_h - 20 - back_h - gap;   // leave room for the Back bar
    int th  = (grid_bot - top - gap) / 2;
    int x0 = L.margin, x1 = L.margin + tw + gap;
    int y0 = top,      y1 = top + th + gap;

    // Brightness tile — tap cycles levels; bar shows the current.
    lv_obj_t* bt = make_tile(settings_container, x0, y0, tw, th, "Brightness", COL_TEXT, brightness_row_cb);
    bar_brightness = lv_bar_create(bt);
    lv_obj_set_size(bar_brightness, tw - 60, 12);
    lv_bar_set_range(bar_brightness, 0, 100);
    lv_obj_clear_flag(bar_brightness, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_brightness, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_brightness, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_brightness, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_brightness, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_brightness, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_brightness, 6, LV_PART_INDICATOR);

    // Status text tile — tap toggles the whimsical bottom line.
    lv_obj_t* st = make_tile(settings_container, x1, y0, tw, th, "Status text", COL_TEXT, status_toggle_cb);
    lbl_status_toggle = lv_label_create(st);
    lv_obj_set_style_text_font(lbl_status_toggle, L.bt_device_font, 0);
    lv_label_set_text(lbl_status_toggle, "On");

    // Clock tile — opens the clock config overlay.
    lv_obj_t* ct = make_tile(settings_container, x0, y1, tw, th, "Clock", COL_TEXT, clock_tile_cb);
    lbl_clock_tile = lv_label_create(ct);
    lv_obj_set_style_text_font(lbl_clock_tile, L.bt_device_font, 0);
    lv_label_set_text(lbl_clock_tile, "Off");

    make_tile(settings_container, x1, y1, tw, th, "Wi-Fi setup", COL_AMBER, wifi_setup_cb);

    // Full-width Back bar.
    make_settings_row(settings_container, grid_bot + gap, L.set_row_h, "Back to Usage", COL_TEXT, true, back_click_cb);

    // ---- Confirmation overlay for Wi-Fi setup (opaque, covers the rows) ----
    settings_confirm = lv_obj_create(settings_container);
    lv_obj_set_size(settings_confirm, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_confirm, 0, 0);
    lv_obj_set_style_bg_color(settings_confirm, COL_BG, 0);
    lv_obj_set_style_bg_opa(settings_confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_confirm, 0, 0);
    lv_obj_clear_flag(settings_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* q = lv_label_create(settings_confirm);
    lv_label_set_text(q, "Open Wi-Fi setup?");
    lv_obj_set_style_text_font(q, L.bt_device_font, 0);
    lv_obj_set_style_text_color(q, COL_TEXT, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, L.content_y);

    lv_obj_t* sub = lv_label_create(settings_confirm);
    lv_label_set_text(sub, "Starts a hotspot and drops Wi-Fi\nuntil you reconnect from a phone.");
    lv_obj_set_style_text_font(sub, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(sub, COL_DIM, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, L.content_w);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, L.content_y + 44);

    int cy = L.content_y + 150;
    make_settings_row(settings_confirm, cy, L.set_row_h, "Yes, open Wi-Fi setup", COL_AMBER, true, confirm_yes_cb);
    make_settings_row(settings_confirm, cy + L.set_row_h + 12, L.set_row_h, "Cancel", COL_TEXT, true, confirm_cancel_cb);

    lv_obj_add_flag(settings_confirm, LV_OBJ_FLAG_HIDDEN);

    // ---- Clock config overlay ----
    settings_clock = lv_obj_create(settings_container);
    lv_obj_set_size(settings_clock, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_clock, 0, 0);
    lv_obj_set_style_bg_color(settings_clock, COL_BG, 0);
    lv_obj_set_style_bg_opa(settings_clock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_clock, 0, 0);
    lv_obj_clear_flag(settings_clock, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ktitle = lv_label_create(settings_clock);
    lv_label_set_text(ktitle, "Clock");
    lv_obj_set_style_text_font(ktitle, L.bt_title_font, 0);
    lv_obj_set_style_text_color(ktitle, COL_TEXT, 0);
    lv_obj_align(ktitle, LV_ALIGN_TOP_MID, 16, L.title_y);

    int kh    = L.set_row_h - 20;   // compact rows so all five fit
    int kstep = kh + 10;
    int ky    = L.content_y;

    // Enable row (label + On/Off), tap toggles.
    lv_obj_t* er = make_settings_row(settings_clock, ky, kh, "Show clock", COL_TEXT, false, clock_enable_cb);
    lbl_clock_enable = lv_label_create(er);
    lv_obj_set_style_text_font(lbl_clock_enable, L.bt_device_font, 0);
    lv_label_set_text(lbl_clock_enable, "Off");
    ky += kstep;

    // UTC offset stepper: [ - ]  UTC+5:30  [ + ]  (centered).
    lv_obj_t* orow = lv_obj_create(settings_clock);
    lv_obj_set_pos(orow, L.margin, ky);
    lv_obj_set_size(orow, L.content_w, kh);
    lv_obj_set_style_bg_color(orow, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(orow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(orow, 8, 0);
    lv_obj_set_style_border_width(orow, 0, 0);
    lv_obj_set_style_pad_column(orow, 22, 0);
    lv_obj_clear_flag(orow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(orow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(orow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_step_btn(orow, kh, "-", clock_off_dec_cb);
    lbl_clock_offset = lv_label_create(orow);
    lv_obj_set_style_text_font(lbl_clock_offset, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_clock_offset, COL_TEXT, 0);
    lv_label_set_text(lbl_clock_offset, "UTC+0:00");
    make_step_btn(orow, kh, "+", clock_off_inc_cb);
    ky += kstep;

    // Time format row (label + "24-hour"/"12-hour"), tap toggles.
    lv_obj_t* fr = make_settings_row(settings_clock, ky, kh, "Time format", COL_TEXT, false, clock_fmt_cb);
    lbl_clock_fmt = lv_label_create(fr);
    lv_obj_set_style_text_font(lbl_clock_fmt, L.bt_device_font, 0);
    lv_label_set_recolor(lbl_clock_fmt, true);   // highlight the active option
    lv_label_set_text(lbl_clock_fmt, "24h / 12h");
    ky += kstep;

    // Show seconds row (label + On/Off), tap toggles.
    lv_obj_t* sr = make_settings_row(settings_clock, ky, kh, "Show seconds", COL_TEXT, false, clock_secs_cb);
    lbl_clock_secs = lv_label_create(sr);
    lv_obj_set_style_text_font(lbl_clock_secs, L.bt_device_font, 0);
    lv_label_set_text(lbl_clock_secs, "Off");
    ky += kstep;

    make_settings_row(settings_clock, ky, kh, "Back", COL_TEXT, true, clock_back_cb);

    lv_obj_add_flag(settings_clock, LV_OBJ_FLAG_HIDDEN);

    settings_refresh();
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_settings_screen(scr);
    init_wifi_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

    // Apply the persisted "status text" preference (loaded via settings_init()).
    s_show_status_text = settings_status_text();
    apply_status_text();
    settings_refresh();
}

// A slot's primary bar fill 0..100 regardless of metric kind.
// Render one metric block on a card; hides the block when !present.
static void render_metric(MetricRow* m, const Metric* d, uint32_t brand) {
    if (!d->present) {
        lv_obj_add_flag(m->panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(m->panel, LV_OBJ_FLAG_HIDDEN);

    char caps[16];
    upcase(d->label, caps, sizeof(caps));
    lv_label_set_text(m->pill, caps);

    char buf[48], num[24];
    int bar = metric_bar_pct(d);
    format_metric_value(d, num, sizeof(num));
    lv_label_set_text(m->val, num);
    if (d->kind == METRIC_PCT) {
        format_reset_time(d->reset_mins, buf, sizeof(buf));
    } else if (d->kind == METRIC_MONEY) {
        if (d->limit > 0.0f)
            snprintf(buf, sizeof(buf), "of $%d.%02d",
                     (int)d->limit, (int)(d->limit * 100) % 100);
        else
            snprintf(buf, sizeof(buf), "No cap");
    } else {   // METRIC_COUNT
        if (d->limit > 0.0f) snprintf(buf, sizeof(buf), "of %d", (int)d->limit);
        else                 snprintf(buf, sizeof(buf), "No cap");
    }
    lv_label_set_text(m->reset, buf);
    lv_bar_set_value(m->bar, bar, LV_ANIM_ON);

    // Same rule as the overview: brand color while it's information, alarm
    // colors once it matters.
    const bool scaled = (d->kind == METRIC_PCT || d->limit > 0.0f);
    lv_obj_set_style_bg_color(m->bar,
        scaled ? meter_color(brand, bar) : COL_BAR_BG, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(m->val,
        (scaled && bar >= 50) ? pct_color((float)bar) : COL_TEXT, 0);
}

void ui_update_providers(void) {
    uint32_t latest_ok = 0;

    for (int c = 0; c < n_cards; c++) {
        ProviderSlot s;
        provider_snapshot(cards[c].slot, &s);
        if (s.last_ok_ms > latest_ok) latest_ok = s.last_ok_ms;

        char buf[48];

        // ---- Card tile ----
        if (s.valid) {
            render_metric(&cards[c].m1, &s.primary,   s.def->color);
            render_metric(&cards[c].m2, &s.secondary, s.def->color);

            // A provider with one window keeps it directly under the header —
            // the blank space belongs at the bottom, not above the number. Only
            // the rule goes, since there is nothing below it to divide.
            if (cards[c].m1.rule) {
                if (s.secondary.present) lv_obj_clear_flag(cards[c].m1.rule, LV_OBJ_FLAG_HIDDEN);
                else                     lv_obj_add_flag(cards[c].m1.rule, LV_OBJ_FLAG_HIDDEN);
            }
        }
        provider_state_text(&s, buf, sizeof(buf));
        lv_label_set_text(cards[c].head_age, buf);
        lv_obj_set_style_text_color(cards[c].head_age,
            s.state == PROV_OK ? COL_DIM : COL_AMBER, 0);
        lv_obj_align(cards[c].head_age, LV_ALIGN_TOP_RIGHT, -L.margin, 4);
    }

    // ---- Overview rows (column 0) ----
    overview_update();

    // Freshness bookkeeping for the idle "Zzz" view and the "Synced" flash.
    static uint32_t prev_latest_ok = 0;
    if (latest_ok) {
        data_received = true;
        if (latest_ok != prev_latest_ok) {
            prev_latest_ok = latest_ok;
            last_data_ms = lv_tick_get();
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !idle_group) return;
    // Usage comes from the Wi-Fi poller, not BLE (which only drives the HID
    // keyboard here). Show the tiles while ANY provider slot has fresh data —
    // freshness is per-slot (3x its own poll interval), so a slow-cadence
    // provider like OpenRouter doesn't flap the idle "Zzz" screen.
    bool any_fresh = false;
    for (int c = 0; c < n_cards && !any_fresh; c++) {
        ProviderSlot s;
        provider_snapshot(cards[c].slot, &s);
        any_fresh = slot_fresh(&s);
    }
    int v = any_fresh ? 2 : 1;
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 2 ? usage_group : idle_group, LV_OBJ_FLAG_HIDDEN);
    // Page dots follow the tiles: no dots on the idle screen.
    for (int i = 0; i < n_dots; i++) {
        if (v == 2) lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: device NTP time + the user's UTC offset (Settings). Redraws
    // only when the displayed field changes; shows "Usage" when disabled or
    // before NTP has synced.
    if (settings_clock_enabled()) {
        time_t t = time(nullptr);
        if (t > 1500000000L) {   // NTP has synced (t is past 2017)
            time_t local = t + (time_t)settings_clock_offset_min() * 60;
            struct tm tmv;
            gmtime_r(&local, &tmv);
            bool secs = settings_clock_seconds();
            int  field = secs ? tmv.tm_sec : tmv.tm_min;   // what triggers a redraw
            if (field != clock_last_sec) {
                clock_last_sec = field;
                char big[8], sub[10];   // big = HH:MM, sub = small :SS / AM-PM
                if (!settings_clock_24h()) {
                    int h12 = tmv.tm_hour % 12;
                    if (h12 == 0) h12 = 12;
                    const char* ap = tmv.tm_hour < 12 ? "AM" : "PM";
                    snprintf(big, sizeof(big), "%d:%02d", h12, tmv.tm_min);
                    if (secs) snprintf(sub, sizeof(sub), ":%02d %s", tmv.tm_sec, ap);
                    else      snprintf(sub, sizeof(sub), " %s", ap);
                } else {
                    snprintf(big, sizeof(big), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                    if (secs) snprintf(sub, sizeof(sub), ":%02d", tmv.tm_sec);
                    else      sub[0] = '\0';
                }
                lv_label_set_text(lbl_title, big);
                lv_label_set_text(lbl_title_small, sub);
            }
        }
    } else if (clock_last_sec != -1) {   // just disabled → restore the title
        clock_last_sec = -1;
        lv_label_set_text(lbl_title, "Usage");
        lv_label_set_text(lbl_title_small, "");
    }

    if (!s_show_status_text) return;   // user disabled the whimsical status line

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Driven by Wi-Fi data freshness (not BLE): whimsical
    // messages while data is live, "Listening/No data" while waiting for a poll.
    const char* text;
    if (view_state == 1) {             // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - last_data_ms < 5000) {
        text = "Synced";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (s_nav_locked) return;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

static void wifi_hotspot_click_cb(lv_event_t* e) {
    (void)e;
    s_hotspot_requested = true;
}

// Leave the Wi-Fi screen. If the hotspot is up, nav is locked and there's no
// clean teardown for the captive portal, so we reboot — the device reconnects
// with the saved creds and lands back on the normal screen. Otherwise (reached
// via PWR cycle) just go back to Usage.
static void wifi_back_cb(lv_event_t* e) {
    (void)e;
    if (s_portal_active) s_reboot_requested = true;   // loop() calls ESP.restart()
    else                 ui_show_screen(SCREEN_USAGE);
}

bool ui_hotspot_requested(void) {
    if (!s_hotspot_requested) return false;
    s_hotspot_requested = false;
    return true;
}

bool ui_reboot_requested(void) {
    if (!s_reboot_requested) return false;
    s_reboot_requested = false;
    return true;
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:   splash_show(); break;
    case SCREEN_USAGE:    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SETTINGS: settings_refresh();  // reflect live brightness/toggle on entry
                          if (settings_confirm) lv_obj_add_flag(settings_confirm, LV_OBJ_FLAG_HIDDEN);
                          if (settings_clock)   lv_obj_add_flag(settings_clock, LV_OBJ_FLAG_HIDDEN);
                          lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_WIFI:     lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_set_nav_locked(bool locked) { s_nav_locked = locked; }

void ui_cycle_screen(void) {
    if (s_nav_locked) return;   // held on the Wi-Fi screen while the hotspot is up
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:    next = SCREEN_SETTINGS; break;
    case SCREEN_SETTINGS: next = SCREEN_WIFI;     break;
    case SCREEN_WIFI:     next = SCREEN_USAGE;    break;
    default:              next = SCREEN_USAGE;    break;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_set_status(ui_status_level_t level, const char* msg) {
    if (!msg || msg[0] == '\0' || level == UI_STATUS_NONE) {
        lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
        if (s_show_status_text) lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_color_t col;
    switch (level) {
        case UI_STATUS_ERROR: col = COL_RED;   break;
        case UI_STATUS_WARN:  col = COL_AMBER; break;
        default:              col = COL_DIM;   break;
    }
    lv_obj_set_style_text_color(lbl_status, col, 0);
    lv_label_set_text(lbl_status, msg);
    lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_wifi_creds(bool portal_active) {
    s_portal_active = portal_active;
    String ssid  = provisioning_get_ssid();
    String pass  = provisioning_get_pass();

    char ssid_buf[20];
    if (ssid.length() == 0)       strlcpy(ssid_buf, "(none)", sizeof(ssid_buf));
    else if (ssid.length() <= 16) strlcpy(ssid_buf, ssid.c_str(), sizeof(ssid_buf));
    else                          snprintf(ssid_buf, sizeof(ssid_buf), "%.13s...", ssid.c_str());
    lv_label_set_text(lbl_wifi_ssid_val, ssid_buf);

    char buf[32];
    redact_password(pass, buf, sizeof(buf));
    lv_label_set_text(lbl_wifi_pass_val, buf);

    // Config row: the LAN address of the on-device config page. mDNS
    // (agentmeter.local) doesn't resolve on every client, so the raw IP is
    // the ground truth; the note offers the friendly name as an alternate.
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        snprintf(buf, sizeof(buf), "http://%s", WiFi.localIP().toString().c_str());
    } else {
        strlcpy(buf, "(waiting for Wi-Fi)", sizeof(buf));
    }
    lv_label_set_text(lbl_wifi_token_val, buf);
    lv_label_set_text(lbl_wifi_code_val, config_server_code());

    lv_label_set_text(lbl_wifi_note, portal_active
        ? "Join Wi-Fi: Agentmeter\nthen open 192.168.4.1"
        : (connected
            ? "Config: any device on this network\n(or agentmeter.local)"
            : "Configure via serial at 115200 baud"));
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}

void ui_set_refresh_progress(int pct) {
    if (!arc_refresh || pct == s_refresh_pct) return;   // only redraw on change
    s_refresh_pct = pct;
    lv_arc_set_value(arc_refresh, pct);
}
