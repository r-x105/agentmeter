#include "ui_common.h"
#include <Arduino.h>

Layout L = {};

void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 138;   // shrunk from 150 to fit the card-header row
        L.usage_panel_gap = 12;
        L.usage_bar_y = 52;
        L.usage_reset_y = 88;
        L.card_head_h = 34;
        L.ov_row_h = 74;
        L.card_head_font = &font_styrene_24;
        L.card_eyebrow_font = &font_styrene_20;
        L.card_val_font    = &font_styrene_64;
        L.card_detail_font = &font_styrene_20;
        L.ov_name_font   = &font_styrene_28;
        L.ov_val_font    = &font_styrene_24;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.wifi_panel_h     = 160;   // 4 rows (SSID/Pass/Config/Code)
        L.wifi_val_x       = 112;   // clears "Config", the widest key
        L.wifi_row_h       = 34;
        L.wifi_btn_h       = 70;
        L.set_row_h        = 74;
        L.set_bar_w        = 120;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 122;
        L.usage_panel_gap = 10;
        L.usage_bar_y = 44;
        L.usage_reset_y = 72;
        L.card_head_h = 34;
        L.ov_row_h = 62;
        L.card_head_font = &font_styrene_20;
        L.card_eyebrow_font = &font_styrene_16;
        L.card_val_font    = &font_styrene_48;
        L.card_detail_font = &font_styrene_16;
        L.ov_name_font   = &font_styrene_20;
        L.ov_val_font    = &font_styrene_16;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.wifi_panel_h     = 136;   // 4 rows (SSID/Pass/Config/Code)
        L.wifi_val_x       = 92;    // clears "Config", the widest key
        L.wifi_row_h       = 28;
        L.wifi_btn_h       = 56;
        L.set_row_h        = 62;
        L.set_bar_w        = 64;
    }

    L.content_w = L.scr_w - 2 * L.margin;
    // The tileview fills the gap between the header and the page dots.
    L.tiles_h = L.scr_h - L.content_y - 58;
    // A card carries a header plus two metric blocks that split what's left.
    L.card_metric_h = (L.tiles_h - L.card_head_h) / 2;
}

lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

lv_color_t meter_color(uint32_t brand, int pct) {
    if (pct >= 80) return COL_RED;
    if (pct >= 50) return COL_AMBER;
    return lv_color_hex(brand);
}

void upcase(const char* in, char* out, size_t len) {
    size_t i = 0;
    for (; in[i] && i + 1 < len; i++)
        out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
    out[i] = '\0';
}

void format_reset_phrase(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "no reset window");
    } else if (mins < 60) {
        snprintf(buf, len, "resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
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

lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
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

lv_obj_t* make_color_dot(lv_obj_t* parent, uint32_t rgb, int d) {
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

int metric_bar_pct(const Metric* m) {
    if (m->kind == METRIC_PCT) return (int)(m->value + 0.5f);
    if (m->limit > 0.0f)       return (int)(m->value / m->limit * 100.0f + 0.5f);
    return 0;   // uncapped MONEY/COUNT — bar stays empty, the number carries it
}

void format_metric_value(const Metric* m, char* buf, size_t len) {
    if (m->kind == METRIC_PCT) {
        snprintf(buf, len, "%d%%", (int)(m->value + 0.5f));
    } else if (m->kind == METRIC_MONEY) {
        snprintf(buf, len, "$%d.%02d", (int)m->value, (int)(m->value * 100) % 100);
    } else {
        snprintf(buf, len, "%d", (int)(m->value + 0.5f));
    }
}

// A slot is "fresh" while its data is younger than 3x its base poll interval.
bool slot_fresh(const ProviderSlot* s) {
    if (!s->valid || s->last_ok_ms == 0) return false;
    return lv_tick_get() - s->last_ok_ms <
           (uint32_t)s->def->poll_interval_s * 1000 * 3;
}

void provider_state_text(const ProviderSlot* s, char* buf, size_t len) {
    switch (s->state) {
        case PROV_AUTH_NEEDED: strlcpy(buf, "re-auth needed", len); return;
        case PROV_LIMITED:     strlcpy(buf, "limit reached", len);  return;
        case PROV_DOWN:        strlcpy(buf, "API down", len);       return;
        case PROV_ERROR:       strlcpy(buf, "error", len);          return;
        default: break;
    }
    if (!slot_fresh(s) && s->last_ok_ms) {
        uint32_t mins = (lv_tick_get() - s->last_ok_ms) / 60000;
        if (mins < 60) snprintf(buf, len, "%lum ago", (unsigned long)mins);
        else           snprintf(buf, len, "%luh ago", (unsigned long)(mins / 60));
        return;
    }
    buf[0] = '\0';
}

lv_color_t provider_state_color(const ProviderSlot* s) {
    return (s->state == PROV_LIMITED || s->state == PROV_AUTH_NEEDED)
        ? COL_RED : COL_AMBER;
}

int enabled_slots(int* out, int max) {
    int n = 0;
    for (int i = 0; i < provider_count() && n < max; i++) {
        ProviderSlot s;
        provider_snapshot(i, &s);
        if (!s.enabled || !s.configured) continue;
        out[n++] = i;
    }
    return n;
}
