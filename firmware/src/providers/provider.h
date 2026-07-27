#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Multi-provider data model. The UI reads ProviderSlot metrics and states —
// never provider names — so a new provider is one .cpp + one registry line.

typedef enum { METRIC_PCT, METRIC_MONEY, METRIC_COUNT } metric_kind_t;

typedef struct {
    bool          present;      // row hidden entirely when false
    metric_kind_t kind;
    float         value;        // 0-100, dollars, or raw count
    float         limit;        // MONEY/COUNT cap (e.g. 50.00); <=0 = uncapped
    int           reset_mins;   // -1 = no reset window
    char          label[12];    // "Session", "Weekly", "Credits"
} Metric;

typedef enum {
    PROV_INIT = 0,      // no successful poll yet
    PROV_OK,
    PROV_AUTH_NEEDED,   // credentials rejected repeatedly; polling stopped
    PROV_LIMITED,       // account usage window exhausted (e.g. Anthropic 429)
    PROV_DOWN,          // provider 5xx
    PROV_ERROR,         // other HTTP / transport failure
} provider_state_t;

struct ProviderDef;

typedef struct ProviderSlot {
    const struct ProviderDef* def;
    bool     enabled;           // user toggle (persisted as <id>.en)
    bool     configured;        // all secret cred fields present in NVS
    Metric   primary, secondary;
    char     status[16];        // provider-specific status word ("allowed", …)
    provider_state_t state;
    bool     valid;             // true once any poll succeeded (metrics usable)
    uint32_t last_ok_ms;        // millis() of last successful poll; 0 = never
    uint32_t next_poll_ms;      // millis() deadline for the next poll
    uint32_t last_poll_ms;      // millis() when the last poll was scheduled
    uint8_t  fail_count;        // consecutive failures → exponential backoff
    uint8_t  auth_fails;        // consecutive 401-class failures → AUTH_NEEDED
    int      last_http_code;
} ProviderSlot;

typedef struct {
    const char* key;      // NVS suffix, <=2 chars ("tk", "rt"): "<id>.<key>"
    const char* label;    // config-page field name ("API key", "Refresh token")
    bool        secret;   // write-only in UI, shown masked
} CredField;

typedef struct ProviderDef {
    const char*      id;              // "anthropic" — NVS prefix, stable forever
    const char*      name;            // "Claude" — card title
    uint32_t         color;           // accent, 24-bit RGB (lv_color_hex)
    const char*      ca_pem;          // named constant from certs.h
    uint16_t         poll_interval_s; // base cadence; backoff multiplies this
    const CredField* creds;
    uint8_t          cred_count;
    // Poller task only. Perform one poll: fill metrics / status / state on the
    // slot. Return true on success (poller resets backoff), false on failure
    // (poller applies backoff). Must complete within HTTP_TIMEOUT_MS + slack.
    bool (*fetch)(ProviderSlot* slot);
} ProviderDef;

// ---- Registry (registry.cpp) ----
int           provider_count(void);
ProviderSlot* provider_slot(int i);              // poller task use; UI must snapshot
void          provider_registry_init(void);      // build slots, load enabled/configured
void          provider_refresh_config(int i);    // re-read NVS after config change; resets auth stop
int           provider_index_by_id(const char* id);   // -1 if unknown

// Thread-safe copy for the UI/main loop (poll task writes slots).
void          provider_snapshot(int i, ProviderSlot* out);
bool          provider_take_new_data(void);      // true once per batch of updates

// Poller task only: commit a locally-updated slot copy back (mutex-guarded).
void          provider_commit(int i, const ProviderSlot* updated);

// Primary provider: drives chime, idle art, and the status line.
int           provider_primary_index(void);      // valid index or -1 (none enabled)
void          provider_set_primary(const char* id);   // persist + recompute
