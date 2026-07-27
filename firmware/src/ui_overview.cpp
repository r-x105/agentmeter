#include "ui_overview.h"
#include "ui_common.h"
#include <Arduino.h>

// Two providers per page. Past that the overview pages vertically rather than
// shrinking rows — a row that has to be squinted at is not worth the space it
// saves. The shorter tiers below still exist for boards without the height.
#define ROWS_PER_PAGE 2
#define ROW_GAP       6      // vertical space between rows
#define ROW_H_CAP     200    // a lone provider doesn't get a 322px row
#define STACK_GAP     8      // between the eyebrow, measurement, bar, detail

// What the detail line carries at each density.
enum {
    DETAIL_NONE = 0,   // no room
    DETAIL_RESET,      // "Resets in 3h 27m"
    DETAIL_FULL,       // "Session · Resets in 3h 27m"
    DETAIL_FULL_SEC,   // "Session · Resets in 3h 27m · wk 8%"
};

// A density tier: one designed layout, not scaled geometry. LVGL fonts are
// pre-baked at fixed sizes, so a fifth density is a new row here rather than
// a new branch in the builder.
//
// `hero` is the tall form: a dim eyebrow, then the measurement alone on its own
// line against a wide left rail. Everything below it is the list form, where the
// name and value share a line. Two forms, four densities.
struct RowTier {
    int16_t          min_h;        // inclusive lower bound on row_h
    const lv_font_t* name_font;    // eyebrow
    const lv_font_t* val_font;
    const lv_font_t* metric_font;  // "Session 2h 25m", beside the value (hero)
    const lv_font_t* detail_font;
    int16_t          bar_h;
    uint8_t          detail_style;
    bool             show_secondary_bar;
    bool             hero;
};

// The hero row is read from across a room, so the sizes below are chosen for
// that distance rather than scaled off the measurement. The big number gives up
// a step (64→48) to pay for it: at arm's length the extra 16px on a two-digit
// percentage was decoration, while the lines that say *which* window and *when*
// it turns over were the ones being squinted at. Everything else moves up.
static const RowTier TIERS[] = {
    /* XXL*/ {150, &font_styrene_24, &font_styrene_48, &font_styrene_28, &font_styrene_28, 10, DETAIL_FULL,     true,  true },
    /* XL */ {130, &font_styrene_24, &font_styrene_48, &font_styrene_24, &font_styrene_24, 8,  DETAIL_FULL,     true,  true },
    /* L  */ {100, &font_styrene_20, &font_styrene_28, &font_styrene_20, &font_styrene_16, 6,  DETAIL_FULL_SEC, false, false},
    /* M  */ { 74, &font_styrene_16, &font_styrene_24, &font_styrene_16, &font_styrene_14, 6,  DETAIL_RESET,    false, false},
    /* S  */ {  0, &font_styrene_16, &font_styrene_20, &font_styrene_16, nullptr,          6,  DETAIL_NONE,     false, false},
};
#define TIER_COUNT ((int)(sizeof(TIERS) / sizeof(TIERS[0])))

#define SEC_BAR_H_HERO 6
#define STACK_GAP_TIGHT 4

struct OverviewRow {
    int              slot;
    const RowTier*   tier;
    lv_obj_t*        panel;
    lv_obj_t*        dot;       // provider identity, before the eyebrow
    lv_obj_t*        name;      // eyebrow, caps, dim
    lv_obj_t*        val;
    lv_obj_t*        metric;    // "Session" — hero only, beside the value
    lv_obj_t*        bar;
    lv_obj_t*        detail;    // dim line, left
    lv_obj_t*        state;     // state string, right end of a line
    lv_obj_t*        glyph;     // "!" (S — no room for words)
    lv_obj_t*        sec_lbl;   // "Weekly 8%" — hero, right of the detail line
    lv_obj_t*        sec_bar;   // second track under the primary — hero only
    int16_t          val_y;     // right-aligned labels are re-aligned on every
    int16_t          state_y;   // update (their width changes), so keep their y
    int16_t          metric_y;
    int16_t          sec_lbl_y;
};

static OverviewRow rows[MAX_PROVIDERS];
static int         n_rows = 0;
static int         n_pages = 1;
static lv_obj_t*   vdots[(MAX_PROVIDERS + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE];
static int         n_vdots = 0;

int overview_page_count(void) { return n_pages; }

// Total height the tier's widget stack needs inside a row.
static int tier_stack_h(const RowTier* t, bool has_sec) {
    const int gap = t->hero ? STACK_GAP : STACK_GAP_TIGHT;
    int h = 0;
    (void)has_sec;
    if (t->hero) h += lv_font_get_line_height(t->name_font) + gap;   // eyebrow
    h += lv_font_get_line_height(t->val_font);                       // measurement
    h += gap + t->bar_h;
    // Hero rows always reserve the second track. A provider's metric shape is
    // not known until its first poll lands, and geometry that depends on data
    // arrival is geometry that is wrong on the first frame.
    if (t->hero) h += 4 + SEC_BAR_H_HERO;
    if (t->detail_style != DETAIL_NONE)
        h += gap + lv_font_get_line_height(t->detail_font);
    return h;
}

// Density for a row of this height. Falls through to the next tier down if the
// chosen stack would not physically fit — self-correcting when a font's line
// height differs from the table's assumptions.
static const RowTier* pick_tier(int row_h, bool has_sec) {
    int content_h = row_h - ROW_GAP;
    int i = 0;
    while (i < TIER_COUNT - 1 && row_h < TIERS[i].min_h) i++;
    while (i < TIER_COUNT - 1 && tier_stack_h(&TIERS[i], has_sec) > content_h) i++;
    return &TIERS[i];
}

// A transparent row on true black — no card fill, no radius. Rows are told
// apart by a hairline rule, which costs one pixel instead of a whole box.
static lv_obj_t* make_row_container(lv_obj_t* parent, int y, int h, bool rule) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_pos(row, L.margin, y);
    lv_obj_set_size(row, L.content_w, h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (rule) {
        lv_obj_t* line = lv_obj_create(row);
        lv_obj_set_size(line, L.content_w, 1);
        lv_obj_set_pos(line, 0, h - 1);
        lv_obj_set_style_bg_color(line, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

static void build_row(lv_obj_t* parent, OverviewRow* r, const ProviderSlot* s,
                      const RowTier* t, int y, int row_h, bool rule) {
    const int panel_h  = row_h - ROW_GAP;
    const int inner_w  = L.content_w;
    const bool has_sec = t->show_secondary_bar && s->secondary.present;
    const int gap      = t->hero ? STACK_GAP : STACK_GAP_TIGHT;

    r->tier = t;
    r->panel = make_row_container(parent, y, panel_h, rule);

    const int name_h  = lv_font_get_line_height(t->name_font);
    const int head_h  = lv_font_get_line_height(t->val_font);
    const int stack_h = tier_stack_h(t, has_sec);
    const int start_y = (panel_h > stack_h) ? (panel_h - stack_h) / 2 : 0;

    char caps[24];
    upcase(s->def->name, caps, sizeof(caps));

    r->dot = make_color_dot(r->panel, s->def->color, 8);
    r->name = lv_label_create(r->panel);
    lv_label_set_text(r->name, caps);
    lv_obj_set_style_text_font(r->name, t->name_font, 0);
    lv_obj_set_style_text_color(r->name, COL_DIM, 0);
    lv_obj_set_style_text_letter_space(r->name, 2, 0);

    r->val = lv_label_create(r->panel);
    lv_label_set_text(r->val, "---");
    lv_obj_set_style_text_font(r->val, t->val_font, 0);
    lv_obj_set_style_text_color(r->val, COL_TEXT, 0);

    r->metric = nullptr;
    r->state  = nullptr;
    r->glyph  = nullptr;

    int y_cur;
    if (t->hero) {
        // Eyebrow, then the measurement alone against a wide left rail.
        lv_obj_set_pos(r->name, 14, start_y);
        lv_obj_set_pos(r->dot, 0, start_y + (name_h - 8) / 2);
        r->val_y = start_y + name_h + gap;
        lv_obj_set_pos(r->val, 0, r->val_y);

        // The window's name and its countdown sit together on the
        // measurement's baseline — "Session 2h 25m". Keeping them on the same
        // line as the number they describe is what frees the line below for
        // the second window.
        r->metric = lv_label_create(r->panel);
        lv_label_set_text(r->metric, "");
        lv_obj_set_style_text_font(r->metric, t->metric_font, 0);
        lv_obj_set_style_text_color(r->metric, COL_DIM, 0);
        r->metric_y = r->val_y + head_h - lv_font_get_line_height(t->metric_font) - 8;
        lv_obj_align(r->metric, LV_ALIGN_TOP_RIGHT, 0, r->metric_y);

        // State words share the eyebrow line, where there is always room.
        r->state = lv_label_create(r->panel);
        lv_label_set_text(r->state, "");
        lv_obj_set_style_text_font(r->state, t->name_font, 0);
        lv_obj_set_style_text_color(r->state, COL_AMBER, 0);
        r->state_y = start_y;
        lv_obj_align(r->state, LV_ALIGN_TOP_RIGHT, 0, r->state_y);

        y_cur = r->val_y + head_h + gap;
    } else {
        // List form: eyebrow left, measurement right, on one line.
        const int line_h = head_h > name_h ? head_h : name_h;
        lv_obj_set_pos(r->name, 14, start_y + (line_h - name_h) / 2);
        lv_obj_set_pos(r->dot, 0, start_y + (line_h - 8) / 2);
        r->val_y = start_y + (line_h - head_h) / 2;
        lv_obj_align(r->val, LV_ALIGN_TOP_RIGHT, 0, r->val_y);
        y_cur = start_y + line_h + gap;
    }

    r->bar = make_bar(r->panel, 0, y_cur, inner_w, t->bar_h);
    lv_obj_set_style_radius(r->bar, t->bar_h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(r->bar, t->bar_h / 2, LV_PART_INDICATOR);
    y_cur += t->bar_h;

    // The second window runs as a thinner track directly under the first, so
    // both read as one instrument rather than two widgets.
    r->sec_lbl = nullptr;
    r->sec_bar = nullptr;
    if (t->hero) {
        y_cur += 4;
        r->sec_bar = make_bar(r->panel, 0, y_cur, inner_w, SEC_BAR_H_HERO);
        lv_obj_set_style_radius(r->sec_bar, SEC_BAR_H_HERO / 2, LV_PART_MAIN);
        lv_obj_set_style_radius(r->sec_bar, SEC_BAR_H_HERO / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(r->sec_bar, LV_OPA_80, LV_PART_INDICATOR);
        y_cur += SEC_BAR_H_HERO;
    }

    r->detail = nullptr;
    if (t->detail_style != DETAIL_NONE) {
        y_cur += gap;
        r->detail = lv_label_create(r->panel);
        lv_label_set_text(r->detail, "");
        lv_obj_set_style_text_font(r->detail, t->detail_font, 0);
        lv_obj_set_style_text_color(r->detail, COL_DIM, 0);
        lv_obj_set_pos(r->detail, 0, y_cur);

        if (t->hero) {
            // The detail line belongs to the second window: its measurement on
            // the left, directly under its own track, and the day it turns over
            // on the right.
            r->sec_lbl = lv_label_create(r->panel);
            lv_label_set_text(r->sec_lbl, "");
            lv_obj_set_style_text_font(r->sec_lbl, t->detail_font, 0);
            lv_obj_set_style_text_color(r->sec_lbl, COL_DIM, 0);
            r->sec_lbl_y = y_cur;
            lv_obj_align(r->sec_lbl, LV_ALIGN_TOP_RIGHT, 0, r->sec_lbl_y);
        } else {
            // List rows have no eyebrow room to spare, so state goes here.
            r->state = lv_label_create(r->panel);
            lv_label_set_text(r->state, "");
            lv_obj_set_style_text_font(r->state, t->detail_font, 0);
            lv_obj_set_style_text_color(r->state, COL_AMBER, 0);
            r->state_y = y_cur;
            lv_obj_align(r->state, LV_ALIGN_TOP_RIGHT, 0, r->state_y);
        }
    } else {
        // Shortest rows have no line to spare — state degrades to a glyph.
        r->glyph = lv_label_create(r->panel);
        lv_label_set_text(r->glyph, "!");
        lv_obj_set_style_text_font(r->glyph, t->name_font, 0);
        lv_obj_set_style_text_color(r->glyph, COL_AMBER, 0);
        lv_obj_add_flag(r->glyph, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_vdots(lv_obj_t* dot_parent) {
    n_vdots = 0;
    if (n_pages <= 1) return;
    const int d = 8, gap = 8;
    const int total = n_pages * d + (n_pages - 1) * gap;
    const int x = L.scr_w - L.margin - d;
    const int y0 = L.content_y + (L.tiles_h - total) / 2;
    for (int i = 0; i < n_pages; i++) {
        vdots[i] = make_color_dot(dot_parent, 0x000000, d);
        lv_obj_set_pos(vdots[i], x, y0 + i * (d + gap));
    }
    n_vdots = n_pages;
    overview_set_active(0, 0);
}

void overview_build(lv_obj_t* tiles_view, lv_obj_t* dot_parent) {
    for (int i = 0; i < n_vdots; i++) lv_obj_delete(vdots[i]);
    n_vdots = 0;
    n_rows  = 0;

    int slots[MAX_PROVIDERS];
    const int n = enabled_slots(slots, MAX_PROVIDERS);
    n_pages = n > 0 ? (n + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE : 1;

    for (int page = 0; page < n_pages; page++) {
        lv_dir_t dir = LV_DIR_NONE;
        if (page == 0 && n > 0)       dir = (lv_dir_t)(dir | LV_DIR_RIGHT);
        if (page > 0)                 dir = (lv_dir_t)(dir | LV_DIR_TOP);
        if (page + 1 < n_pages)       dir = (lv_dir_t)(dir | LV_DIR_BOTTOM);
        lv_obj_t* tile = lv_tileview_add_tile(tiles_view, 0, page, dir);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        const int first = page * ROWS_PER_PAGE;
        const int count = (n - first) < ROWS_PER_PAGE ? (n - first) : ROWS_PER_PAGE;
        if (count <= 0) continue;

        int row_h = L.tiles_h / count;
        if (row_h > ROW_H_CAP) row_h = ROW_H_CAP;
        const int y0 = (L.tiles_h - row_h * count) / 2;

        // One tier per page, so rows on a page share a visual language. A page
        // counts as having secondaries if any of its providers reports one.
        bool page_has_sec = false;
        for (int i = 0; i < count; i++) {
            ProviderSlot s;
            provider_snapshot(slots[first + i], &s);
            if (s.secondary.present) { page_has_sec = true; break; }
        }
        const RowTier* tier = pick_tier(row_h, page_has_sec);

        for (int i = 0; i < count; i++) {
            ProviderSlot s;
            provider_snapshot(slots[first + i], &s);
            OverviewRow* r = &rows[n_rows++];
            r->slot = slots[first + i];
            build_row(tile, r, &s, tier, y0 + i * row_h, row_h, i + 1 < count);
        }
    }

    build_vdots(dot_parent);
    overview_update();
}

void overview_update(void) {
    char buf[64], num[24];

    for (int i = 0; i < n_rows; i++) {
        OverviewRow* r = &rows[i];
        ProviderSlot s;
        provider_snapshot(r->slot, &s);
        const Metric* p = &s.primary;
        const RowTier* t = r->tier;

        // ---- Primary value + bar ----
        if (s.valid && p->present) {
            format_metric_value(p, num, sizeof(num));
            lv_label_set_text(r->val, num);
            const int bar = metric_bar_pct(p);
            const bool scaled = (p->kind == METRIC_PCT || p->limit > 0.0f);
            lv_bar_set_value(r->bar, bar, LV_ANIM_ON);
            lv_obj_set_style_bg_color(r->bar,
                scaled ? meter_color(s.def->color, bar) : COL_BAR_BG,
                LV_PART_INDICATOR);
            // The measurement stays white while it is merely information, and
            // takes the alarm color only once the bar has one.
            lv_obj_set_style_text_color(r->val,
                (scaled && bar >= 50) ? pct_color((float)bar) : COL_TEXT, 0);
            // "Session 2h 25m" — the window's name and its countdown together,
            // so the primary window is fully described on its own line.
            if (r->metric) {
                char when[24];
                format_reset_short(p->reset_mins, when, sizeof(when));
                if (when[0]) snprintf(buf, sizeof(buf), "%s %s", p->label, when);
                else         snprintf(buf, sizeof(buf), "%s", p->label);
                lv_label_set_text(r->metric, buf);
            }
        } else {
            lv_label_set_text(r->val, "---");
            lv_obj_set_style_text_color(r->val, COL_DIM, 0);
            lv_bar_set_value(r->bar, 0, LV_ANIM_OFF);
            if (r->metric) lv_label_set_text(r->metric, "");
        }
        if (r->metric)
            lv_obj_align(r->metric, LV_ALIGN_TOP_RIGHT, 0, r->metric_y);
        // Hero rows anchor the measurement to the left rail; list rows right-
        // align it, which has to re-run because the value's width changed.
        if (t->hero) lv_obj_set_pos(r->val, 0, r->val_y);
        else         lv_obj_align(r->val, LV_ALIGN_TOP_RIGHT, 0, r->val_y);

        // ---- Detail line ----
        if (r->detail) {
            buf[0] = '\0';
            if (s.valid && p->present) {
                if (t->hero) {
                    // The primary window is fully described on the line above,
                    // so this line reads out the second one — "Weekly 13%". A
                    // provider with a single window leaves it empty rather than
                    // repeating what the measurement line already said.
                    if (s.secondary.present) {
                        char sv[24];
                        format_metric_value(&s.secondary, sv, sizeof(sv));
                        snprintf(buf, sizeof(buf), "%s %s", s.secondary.label, sv);
                    }
                } else if (t->detail_style == DETAIL_RESET) {
                    format_reset_time(p->reset_mins, buf, sizeof(buf));
                } else {
                    char phrase[40];
                    format_reset_phrase(p->reset_mins, phrase, sizeof(phrase));
                    if (t->detail_style == DETAIL_FULL_SEC && s.secondary.present) {
                        char sv[24];
                        format_metric_value(&s.secondary, sv, sizeof(sv));
                        snprintf(buf, sizeof(buf), "%s %s, %s %s",
                                 p->label, phrase, s.secondary.label, sv);
                    } else {
                        snprintf(buf, sizeof(buf), "%s %s", p->label, phrase);
                    }
                }
            }
            lv_label_set_text(r->detail, buf);
        }

        // ---- Secondary track ----
        // The row always reserves this space so nothing reflows when a poll
        // lands, but a provider with one window shows one track, not an empty
        // second one.
        if (r->sec_bar) {
            if (s.valid && s.secondary.present) {
                lv_obj_clear_flag(r->sec_bar, LV_OBJ_FLAG_HIDDEN);
                const int sbar = metric_bar_pct(&s.secondary);
                const bool sscaled = (s.secondary.kind == METRIC_PCT || s.secondary.limit > 0.0f);
                lv_bar_set_value(r->sec_bar, sbar, LV_ANIM_ON);
                lv_obj_set_style_bg_color(r->sec_bar,
                    sscaled ? meter_color(s.def->color, sbar) : COL_BAR_BG,
                    LV_PART_INDICATOR);
            } else {
                lv_obj_add_flag(r->sec_bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // The second window's readout is a live measurement, not a caption, so
        // it reads at full brightness and takes the alarm color along with its
        // own track — the same rule the primary value follows above.
        if (r->detail && t->hero) {
            const int  sbar    = metric_bar_pct(&s.secondary);
            const bool sscaled = (s.secondary.kind == METRIC_PCT || s.secondary.limit > 0.0f);
            lv_obj_set_style_text_color(r->detail,
                (s.valid && s.secondary.present && sscaled && sbar >= 50)
                    ? pct_color((float)sbar) : COL_TEXT, 0);
        }

        // A seven-day window turns over on a day, not in a number of hours.
        if (r->sec_lbl) {
            buf[0] = '\0';
            if (s.valid && s.secondary.present)
                format_reset_weekday(s.secondary.reset_mins, buf, sizeof(buf));
            lv_label_set_text(r->sec_lbl, buf);
            lv_obj_align(r->sec_lbl, LV_ALIGN_TOP_RIGHT, 0, r->sec_lbl_y);
        }

        // ---- State: words where there's room, a glyph where there isn't ----
        provider_state_text(&s, buf, sizeof(buf));
        if (r->state) {
            lv_label_set_text(r->state, buf);
            lv_obj_set_style_text_color(r->state, provider_state_color(&s), 0);
            lv_obj_align(r->state, LV_ALIGN_TOP_RIGHT, 0, r->state_y);
        } else if (r->glyph) {
            if (buf[0] == '\0') {
                lv_obj_add_flag(r->glyph, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(r->glyph, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(r->glyph, provider_state_color(&s), 0);
                lv_obj_align_to(r->glyph, r->val, LV_ALIGN_OUT_LEFT_MID, -10, 0);
            }
        }
    }
}

void overview_set_active(int col, int page) {
    for (int i = 0; i < n_vdots; i++) {
        if (col != 0) {
            lv_obj_add_flag(vdots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(vdots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(vdots[i], i == page ? COL_TEXT : COL_BAR_BG, 0);
        }
    }
}
