#pragma once
#include <Arduino.h>

// Per-provider credential + flag storage. NVS namespace "agentmeter", keys
// "<id>.<suffix>" with 2-char suffixes so every planned id fits the 15-char
// NVS key limit ("openrouter.tk" = 13).
//
// Suffix convention:
//   tk  API key / long-lived token       en  enabled flag ("1"/"0")
//   rt  OAuth refresh token              at  cached OAuth access token
//   pr  previous refresh token           ex  access-token expiry (unix str)
//
// Values are cached in RAM behind a mutex (poller task reads, main loop /
// config server writes) — same pattern as provisioning.cpp, which keeps
// owning the Wi-Fi SSID/pass in the legacy "clawdmeter" namespace.

void   secrets_init(void);   // load cache; one-time migration of the legacy
                             // clawdmeter "token" key -> anthropic.tk

String secrets_get(const char* provider_id, const char* suffix);
bool   secrets_has(const char* provider_id, const char* suffix);

// secrets_set/erase update the RAM cache immediately (readers see the new
// value at once) and QUEUE the NVS write; secrets_flush() — called every
// main-loop tick — performs the actual flash writes. This split exists
// because the poll task's stack lives in PSRAM and a flash write from it
// asserts (cache-disabled PSRAM access) — see poller.cpp. Callable from any
// task; flush only from the main loop.
void   secrets_set(const char* provider_id, const char* suffix, const char* value);
void   secrets_erase(const char* provider_id, const char* suffix);
void   secrets_flush(void);

bool   secrets_enabled(const char* provider_id);          // default: enabled iff "tk"/"rt" present
void   secrets_set_enabled(const char* provider_id, bool on);
