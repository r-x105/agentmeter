#pragma once
#include "provider.h"

// Pure parsing for the Anthropic provider — no Arduino types, native-testable.
// Inputs are the raw anthropic-ratelimit-unified-* response header values
// ("" when absent) plus the current unix time for reset-countdown math.

#ifdef __cplusplus
extern "C" {
#endif

// Fill slot metrics from a 200 response's headers. Returns false when the
// utilization headers are missing/garbled (slot left untouched).
bool anthropic_parse_ok(const char* h5_util, const char* h7_util,
                        const char* h_status,
                        const char* h5_reset, const char* h7_reset,
                        long now_epoch, ProviderSlot* slot);

// Apply a 429 to the slot: primary (session) bar forced full, real weekly +
// reset kept when the headers carry them, status set to "limited".
void anthropic_apply_limited(const char* h7_util, const char* h5_reset,
                             long now_epoch, ProviderSlot* slot);

// Shared helper: minutes until a unix-timestamp string, -1 when unknown.
int reset_mins_from_unix(const char* ts_str, long now_epoch);

#ifdef __cplusplus
}
#endif
