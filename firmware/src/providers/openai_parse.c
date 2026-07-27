#include "openai_parse.h"
#include "json_mini.h"
#include <string.h>
#include <stdio.h>

// Label a rate window by its length. Codex windows are dynamic per plan —
// a Plus account can have a weekly PRIMARY window with no secondary.
static void window_label(long secs, char* out, int n) {
    if (secs <= 6 * 3600)            snprintf(out, n, "Session");
    else if (secs >= 6 * 86400)      snprintf(out, n, "Weekly");
    else if (secs % 86400 == 0)      snprintf(out, n, "%ldd", secs / 86400);
    else                             snprintf(out, n, "%ldh", secs / 3600);
}

// Parse one {"used_percent":…,"limit_window_seconds":…,"reset_after_seconds":…}
// scoped at `win` (a pointer INTO the body, so json_mini scans forward from
// the window's own key — the same field names repeat in both windows).
static bool parse_window(const char* win, Metric* m) {
    float used = 0, wsecs = 0, rsecs = -1;
    if (!json_find_number(win, "used_percent", &used)) return false;
    json_find_number(win, "limit_window_seconds", &wsecs);
    json_find_number(win, "reset_after_seconds", &rsecs);

    m->present    = true;
    m->kind       = METRIC_PCT;
    m->value      = used;
    m->limit      = 100.0f;
    m->reset_mins = rsecs >= 0 ? (int)(rsecs / 60) : -1;
    window_label((long)wsecs, m->label, sizeof(m->label));
    return true;
}

bool openai_parse_usage(const char* body, ProviderSlot* slot, bool* limited) {
    const char* prim = strstr(body, "\"primary_window\"");
    if (!prim || json_is_null(prim, "primary_window")) return false;
    if (!parse_window(prim, &slot->primary)) return false;

    const char* sec = strstr(body, "\"secondary_window\"");
    if (sec && !json_is_null(sec, "secondary_window") &&
        parse_window(sec, &slot->secondary)) {
        // both windows live
    } else {
        slot->secondary.present = false;
    }

    // limit_reached is a bare bool (json_mini has no bool getter) — scan it.
    bool lim = false;
    const char* lr = strstr(body, "\"limit_reached\"");
    if (lr) {
        const char* v = strchr(lr + 15, ':');
        if (v) { while (*++v == ' ') {} lim = strncmp(v, "true", 4) == 0; }
    }
    *limited = lim;
    snprintf(slot->status, sizeof(slot->status), lim ? "limited" : "allowed");
    return true;
}

bool openai_parse_refresh(const char* body,
                          char* at_out, int at_n,
                          char* rt_out, int rt_n,
                          long* expires_in) {
    float exp = 0;
    if (!json_find_string(body, "access_token", at_out, at_n))  return false;
    if (!json_find_string(body, "refresh_token", rt_out, rt_n)) return false;
    if (!json_find_number(body, "expires_in", &exp))            return false;
    *expires_in = (long)exp;
    return true;
}
