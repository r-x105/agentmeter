#pragma once
#include "provider.h"

// Pure parsing for the OpenAI/Codex provider — native-testable.
//
// Usage payload: GET https://chatgpt.com/backend-api/wham/usage →
//   {"plan_type":"plus","rate_limit":{"allowed":true,"limit_reached":false,
//     "primary_window":{"used_percent":0,"limit_window_seconds":604800,
//                       "reset_after_seconds":604800,"reset_at":…},
//     "secondary_window":null | {…same fields…}}, …}
// Windows are DYNAMIC per account/plan: label comes from
// limit_window_seconds, never assumed 5h/weekly (verified 2026-07-26 on a
// Plus account whose primary window is weekly). reset_after_seconds is
// relative, so no wall clock is needed.
//
// Refresh payload: POST https://auth.openai.com/oauth/token →
//   {"access_token":"eyJ…","refresh_token":"rt.1.…","expires_in":864000,…}

#ifdef __cplusplus
extern "C" {
#endif

// Fill slot metrics from a 200 wham/usage body. Returns false when the
// primary window is absent/garbled (slot untouched). Sets *limited when the
// account has hit its window (limit_reached / !allowed).
bool openai_parse_usage(const char* body, ProviderSlot* slot, bool* limited);

// Extract the rotated credentials from a 200 refresh body. Returns false
// unless BOTH tokens and expires_in are present.
bool openai_parse_refresh(const char* body,
                          char* at_out, int at_n,
                          char* rt_out, int rt_n,
                          long* expires_in);

#ifdef __cplusplus
}
#endif
