#include "openrouter_parse.h"
#include "json_mini.h"
#include <stdio.h>

bool openrouter_parse_ok(const char* body, ProviderSlot* slot) {
    float usage = 0.0f;
    if (!json_find_number(body, "usage", &usage)) return false;

    float limit = 0.0f;
    bool has_limit = json_find_number(body, "limit", &limit) && limit > 0.0f;

    slot->primary.present    = true;
    slot->primary.kind       = METRIC_MONEY;
    slot->primary.value      = usage;
    slot->primary.limit      = has_limit ? limit : 0.0f;   // <=0 = uncapped
    slot->primary.reset_mins = -1;                          // credits don't reset
    snprintf(slot->primary.label, sizeof(slot->primary.label), "Credits");

    slot->secondary.present = false;
    snprintf(slot->status, sizeof(slot->status), "%s",
             (has_limit && usage >= limit) ? "limited" : "allowed");
    return true;
}
