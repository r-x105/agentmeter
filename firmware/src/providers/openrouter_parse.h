#pragma once
#include "provider.h"

// Pure parsing for the OpenRouter provider — native-testable.
// Payload: GET https://openrouter.ai/api/v1/key →
//   {"data":{"label":"sk-or-v1-…","usage":12.34,"limit":50.0,          // or null
//            "is_free_tier":false, …}}
// "limit": null means uncapped credits.

#ifdef __cplusplus
extern "C" {
#endif

// Fill slot metrics from a 200 body. Returns false when "usage" is absent.
bool openrouter_parse_ok(const char* body, ProviderSlot* slot);

#ifdef __cplusplus
}
#endif
