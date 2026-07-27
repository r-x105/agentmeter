#pragma once
#include <stdbool.h>

// Minimal JSON field extraction for small, known provider payloads — avoids
// an ArduinoJson dependency. Pure C, no Arduino types, native-testable.
// Not a JSON parser: does a key scan, so only use on payloads whose shape we
// control the expectations of (each provider's fixture-tested endpoint).

#ifdef __cplusplus
extern "C" {
#endif

// Find "key": <number> and store it. Returns false if the key is absent or
// its value is not a number (e.g. null). Matches the first occurrence.
bool json_find_number(const char* body, const char* key, float* out);

// Find "key": "<string>" and copy at most n-1 chars. Returns false if absent
// or not a string.
bool json_find_string(const char* body, const char* key, char* out, int n);

// True if "key": null (explicitly null, distinct from absent).
bool json_is_null(const char* body, const char* key);

#ifdef __cplusplus
}
#endif
