#include "anthropic_parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int reset_mins_from_unix(const char* ts_str, long now_epoch) {
    if (!ts_str || !*ts_str) return -1;
    if (now_epoch < 1000000000L) return -1;  // NTP not synced yet
    long ts = strtol(ts_str, NULL, 10);
    if (ts <= 0) return -1;
    long diff = ts - now_epoch;
    return diff > 0 ? (int)(diff / 60) : 0;
}

static void set_pct(Metric* m, const char* label, float pct, int reset_mins) {
    m->present    = true;
    m->kind       = METRIC_PCT;
    m->value      = pct;
    m->limit      = 100.0f;
    m->reset_mins = reset_mins;
    snprintf(m->label, sizeof(m->label), "%s", label);
}

bool anthropic_parse_ok(const char* h5_util, const char* h7_util,
                        const char* h_status,
                        const char* h5_reset, const char* h7_reset,
                        long now_epoch, ProviderSlot* slot) {
    if (!h5_util || !*h5_util) return false;
    char* end = NULL;
    float s = strtof(h5_util, &end);
    if (end == h5_util) return false;
    float w = (h7_util && *h7_util) ? strtof(h7_util, NULL) : 0.0f;

    set_pct(&slot->primary,   "Session", s * 100.0f,
            reset_mins_from_unix(h5_reset, now_epoch));
    set_pct(&slot->secondary, "Weekly",  w * 100.0f,
            reset_mins_from_unix(h7_reset, now_epoch));
    snprintf(slot->status, sizeof(slot->status), "%s",
             (h_status && *h_status) ? h_status : "allowed");
    return true;
}

void anthropic_apply_limited(const char* h7_util, const char* h5_reset,
                             long now_epoch, ProviderSlot* slot) {
    // Preserve last-known metrics as the base; overwrite what the 429 tells us.
    if (h7_util && *h7_util) {
        set_pct(&slot->secondary, "Weekly", strtof(h7_util, NULL) * 100.0f,
                slot->secondary.reset_mins);
    }
    int r = reset_mins_from_unix(h5_reset, now_epoch);
    set_pct(&slot->primary, "Session", 100.0f,   // window spent — show it full
            r >= 0 ? r : slot->primary.reset_mins);
    snprintf(slot->status, sizeof(slot->status), "limited");
}
