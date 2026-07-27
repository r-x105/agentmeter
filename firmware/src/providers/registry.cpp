#include "provider.h"
#include "secrets.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Compiled-in provider set. Adding a provider: one providers/<name>.cpp
// defining a ProviderDef, one extern + one array entry here.
extern const ProviderDef ANTHROPIC_DEF;
extern const ProviderDef OPENAI_DEF;
extern const ProviderDef OPENROUTER_DEF;

static const ProviderDef* DEFS[] = {
    &ANTHROPIC_DEF,
    &OPENAI_DEF,
    &OPENROUTER_DEF,
};
#define N_PROVIDERS ((int)(sizeof(DEFS) / sizeof(DEFS[0])))

static ProviderSlot      s_slots[N_PROVIDERS];
static SemaphoreHandle_t s_mutex = nullptr;
static volatile bool     s_has_new = false;
static int               s_primary = -1;   // index; -1 = none enabled

int           provider_count(void)   { return N_PROVIDERS; }
ProviderSlot* provider_slot(int i)   { return &s_slots[i]; }

int provider_index_by_id(const char* id) {
    for (int i = 0; i < N_PROVIDERS; i++)
        if (strcmp(DEFS[i]->id, id) == 0) return i;
    return -1;
}

static bool slot_configured(const ProviderDef* def) {
    for (int c = 0; c < def->cred_count; c++)
        if (!secrets_has(def->id, def->creds[c].key)) return false;
    return true;
}

static void recompute_primary(void) {
    // Explicit user choice first (persisted id), else first enabled slot.
    Preferences prefs;
    prefs.begin("agentmeter", true);
    String want = prefs.getString("primary", "");
    prefs.end();

    s_primary = -1;
    if (want.length()) {
        int i = provider_index_by_id(want.c_str());
        if (i >= 0 && s_slots[i].enabled && s_slots[i].configured) s_primary = i;
    }
    if (s_primary < 0) {
        for (int i = 0; i < N_PROVIDERS; i++) {
            if (s_slots[i].enabled && s_slots[i].configured) { s_primary = i; break; }
        }
    }
}

void provider_registry_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < N_PROVIDERS; i++) {
        ProviderSlot* s = &s_slots[i];
        memset(s, 0, sizeof(*s));
        s->def        = DEFS[i];
        s->enabled    = secrets_enabled(DEFS[i]->id);
        s->configured = slot_configured(DEFS[i]);
        s->state      = PROV_INIT;
        s->primary.reset_mins = s->secondary.reset_mins = -1;
        Serial.printf("registry: %s enabled=%d configured=%d\n",
            DEFS[i]->id, s->enabled, s->configured);
    }
    recompute_primary();
}

void provider_refresh_config(int i) {
    if (i < 0 || i >= N_PROVIDERS) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ProviderSlot* s = &s_slots[i];
    s->enabled    = secrets_enabled(s->def->id);
    s->configured = slot_configured(s->def);
    // New credentials lift an auth stop and any backoff; poll promptly.
    s->auth_fails = 0;
    s->fail_count = 0;
    if (s->state == PROV_AUTH_NEEDED) s->state = PROV_INIT;
    s->next_poll_ms = millis();
    recompute_primary();
    xSemaphoreGive(s_mutex);
}

void provider_snapshot(int i, ProviderSlot* out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_slots[i];
    xSemaphoreGive(s_mutex);
}

// Poller task: commit a locally-updated copy back into the shared slot.
void provider_commit(int i, const ProviderSlot* updated) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_slots[i] = *updated;
    s_has_new  = true;
    xSemaphoreGive(s_mutex);
}

bool provider_take_new_data(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool r = s_has_new;
    s_has_new = false;
    xSemaphoreGive(s_mutex);
    return r;
}

int provider_primary_index(void) { return s_primary; }

void provider_set_primary(const char* id) {
    Preferences prefs;
    prefs.begin("agentmeter", false);
    prefs.putString("primary", id);
    prefs.end();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    recompute_primary();
    xSemaphoreGive(s_mutex);
}
