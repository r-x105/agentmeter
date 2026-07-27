#pragma once
#include <lvgl.h>
#include <stddef.h>
#include "theme.h"
#include "hal/board_caps.h"
#include "providers/provider.h"

// Shared UI vocabulary: the computed layout, the color aliases, the widget
// factories, and the provider→text helpers. Everything here is used by more
// than one screen builder (ui.cpp, ui_overview.cpp) — screen-specific code
// stays in its own file.

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_64);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);

// Upper bound on providers the UI will render (overview rows + card tiles).
//
// This is an array bound, NOT a statement of what the hardware can take. LVGL
// allocates every widget from a fixed 48KB pool (LV_MEM_SIZE), and measured on
// the 2.16 with two providers configured that pool has ~8KB free. Rebuilding
// the tiles for a third provider exhausts it and LVGL aborts, which resets the
// device. Raising the real ceiling means moving the LVGL pool to PSRAM or
// building card tiles lazily — not raising this number. The `lvmem` serial
// command reports the pool's current headroom.
#define MAX_PROVIDERS 8

// Design tokens — values live in theme.h
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen (provider tileview)
    int16_t tiles_h;          // height of the tileview zone (all tiles)
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t card_head_h;      // provider name row above a card's metric blocks
    int16_t card_metric_h;    // height of one metric block on a provider card
    int16_t ov_row_h;         // overview: height of one provider row
    const lv_font_t* card_head_font;
    const lv_font_t* card_eyebrow_font;   // "SESSION" / "WEEKLY"
    const lv_font_t* card_val_font;
    const lv_font_t* card_detail_font;
    const lv_font_t* ov_name_font;
    const lv_font_t* ov_val_font;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;

    // Wi-Fi screen
    int16_t wifi_panel_h;
    int16_t wifi_val_x;
    int16_t wifi_row_h;
    int16_t wifi_btn_h;

    // Settings screen
    int16_t set_row_h;
    int16_t set_panel_h;
    int16_t set_btn_h;
    int16_t set_bar_w;   // brightness bar width (kept small so the row never clips)
};
extern Layout L;

void compute_layout(const BoardCaps& c);

// ---- Shared helpers ----
// Alarm colors: green below 50, amber at 50, red at 80.
lv_color_t pct_color(float pct);
// A meter's fill: the provider's own brand color while the number is merely
// information, then amber and red once it matters. Color only appears when it
// means something.
lv_color_t meter_color(uint32_t brand, int pct);
// ASCII-only uppercase for eyebrow labels — the fonts are ASCII subsets, so
// this is a byte-wise transform, not a locale-aware one.
void       upcase(const char* in, char* out, size_t len);
// "Resets in 3h 27m" — standalone, sentence-initial.
void       format_reset_time(int mins, char* buf, size_t len);
// "resets in 3h 27m" — reads as a clause after a metric label ("Session …").
// The bundled fonts are ASCII-only subsets, so separators like U+00B7 render
// as tofu; phrasing carries the join instead.
void       format_reset_phrase(int mins, char* buf, size_t len);

lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h);
lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h);
lv_obj_t* make_color_dot(lv_obj_t* parent, uint32_t rgb, int d);

// ---- Provider → presentation ----
// Bar fill 0..100 for any metric kind; uncapped MONEY/COUNT yields 0 (the
// number carries the row, the bar stays empty).
int  metric_bar_pct(const Metric* m);
// "17%", "$12.50", "42" — the value string for any metric kind.
void format_metric_value(const Metric* m, char* buf, size_t len);
// True while a slot's data is younger than 3x its base poll interval.
bool slot_fresh(const ProviderSlot* s);
// "re-auth needed" / "limit reached" / "12m ago" / "" when healthy and fresh.
void provider_state_text(const ProviderSlot* s, char* buf, size_t len);
// Color for a state string: red for limit/auth, amber for the rest.
lv_color_t provider_state_color(const ProviderSlot* s);
// Registry indices of providers that are enabled AND configured, in order.
// Returns how many were written (never more than max).
int  enabled_slots(int* out, int max);
