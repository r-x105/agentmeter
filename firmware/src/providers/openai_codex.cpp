#include "provider.h"
#include "openai_parse.h"
#include "http.h"
#include "certs.h"
#include "secrets.h"
#include <Arduino.h>
#include <time.h>

// OpenAI / Codex plan limits via GET chatgpt.com/backend-api/wham/usage,
// authenticated with the ChatGPT OAuth access token and kept alive by an
// on-device refresh of the Codex CLI's grant (public client id, verified
// 2026-07-26; access tokens live ~10 days, refresh tokens ROTATE).
//
// Credential lifecycle (NVS suffixes):
//   rt  current refresh token (user pastes once from ~/.codex/auth.json)
//   pr  previous refresh token, kept in case rotation has a grace window
//   at  cached access token        ex  its expiry (unix seconds; 0=unknown)
//
// THE CRITICAL INVARIANT: a rotated refresh token is persisted to NVS
// immediately on receipt — before first use, before anything else. A crash
// between refresh and persist is how other projects bricked this credential
// (decolua/9router#1663). The old token moves to `pr` in the same breath.

#define OPENAI_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define AT_REFRESH_MARGIN_S 3600     // refresh 1h before expiry
#define MAX_AUTH_RETRIES 3

// Single poll task → static buffers are safe and keep the 12 KB stack calm.
static char s_at[2560];
static char s_rt[512];

// One refresh attempt. Returns true when new tokens are cached+persisted.
// On auth-class failure bumps slot->auth_fails (→ AUTH_NEEDED at the cap).
static bool do_refresh(ProviderSlot* slot) {
    String rt = secrets_get("openai", "rt");
    String body = String("{\"client_id\":\"" OPENAI_CLIENT_ID "\","
                         "\"grant_type\":\"refresh_token\","
                         "\"refresh_token\":\"") + rt + "\"}";

    HttpRequest req = {};
    req.host         = "auth.openai.com";
    req.path         = "/oauth/token";
    req.method       = "POST";
    req.body         = body.c_str();
    req.content_type = "application/json";
    req.ca_pem       = CA_GTS_ROOT_R4;

    HttpResponse resp;
    http_perform(&req, &resp);
    slot->last_http_code = resp.code;

    if (resp.code == 200) {
        long expires_in = 0;
        if (!openai_parse_refresh(resp.body.c_str(), s_at, sizeof(s_at),
                                  s_rt, sizeof(s_rt), &expires_in)) {
            slot->state = PROV_ERROR;
            Serial.println("openai: refresh 200 but body unparseable");
            return false;
        }
        // Persist the rotated RT FIRST — before the access token, before use.
        if (rt != s_rt) {
            secrets_set("openai", "pr", rt.c_str());
            secrets_set("openai", "rt", s_rt);
        }
        long now = (long)time(nullptr);
        char ex[16];
        snprintf(ex, sizeof(ex), "%ld",
                 now > 1000000000L ? now + expires_in - AT_REFRESH_MARGIN_S : 0L);
        secrets_set("openai", "at", s_at);
        secrets_set("openai", "ex", ex);
        slot->auth_fails = 0;
        Serial.printf("openai: refreshed (rotated=%d, expires_in=%lds)\n",
                      rt != s_rt, expires_in);
        return true;

    } else if (resp.code >= 400 && resp.code < 500) {
        slot->auth_fails++;
        slot->state = (slot->auth_fails >= MAX_AUTH_RETRIES)
                      ? PROV_AUTH_NEEDED : PROV_ERROR;
        Serial.printf("openai: refresh %d (#%d/%d)%s\n", resp.code,
            slot->auth_fails, MAX_AUTH_RETRIES,
            slot->state == PROV_AUTH_NEEDED ? " — re-provision refresh token" : "");
        return false;

    } else {
        slot->state = resp.code >= 500 ? PROV_DOWN : PROV_ERROR;
        Serial.printf("openai: refresh error %d\n", resp.code);
        return false;
    }
}

// GET wham/usage with the cached access token. Fills the slot on 200.
// Returns the HTTP code (0 on transport failure).
static int do_usage(ProviderSlot* slot) {
    String auth = "Bearer " + secrets_get("openai", "at");

    HttpRequest req = {};
    req.host   = "chatgpt.com";
    req.path   = "/backend-api/wham/usage";
    req.method = "GET";
    req.auth   = auth.c_str();
    req.ca_pem = CA_GTS_ROOT_R4;

    HttpResponse resp;
    http_perform(&req, &resp);
    slot->last_http_code = resp.code;

    if (resp.code == 200) {
        bool limited = false;
        if (!openai_parse_usage(resp.body.c_str(), slot, &limited)) {
            slot->state = PROV_ERROR;
            Serial.println("openai: 200 but body unparseable");
            return resp.code;
        }
        slot->state      = limited ? PROV_LIMITED : PROV_OK;
        slot->valid      = true;
        slot->auth_fails = 0;
        Serial.printf("openai: %s=%.1f%%%s status=%s\n",
            slot->primary.label, slot->primary.value,
            slot->secondary.present ? " (+2nd)" : "", slot->status);
    }
    return resp.code > 0 ? resp.code : 0;
}

static bool fetch(ProviderSlot* slot) {
    // Refresh proactively when the cached access token is missing or near
    // expiry (ex==0 means "age unknown" — poll with it and let a 401 below
    // trigger the refresh; it can't be trusted but usually still works).
    long now = (long)time(nullptr);
    long ex  = secrets_get("openai", "ex").toInt();
    bool have_at = secrets_has("openai", "at");
    if (!have_at || (now > 1000000000L && ex > 0 && now >= ex)) {
        if (!do_refresh(slot)) return false;
    }

    int code = do_usage(slot);
    if (code == 200) return slot->state == PROV_OK || slot->state == PROV_LIMITED;

    if (code == 401 || code == 403) {
        // Stale/revoked access token — one refresh + one retry, then let the
        // refresh path's auth counting decide.
        Serial.printf("openai: usage %d — refreshing access token\n", code);
        if (!do_refresh(slot)) return false;
        code = do_usage(slot);
        if (code == 200) return slot->state == PROV_OK || slot->state == PROV_LIMITED;
        slot->auth_fails++;
        slot->state = (slot->auth_fails >= MAX_AUTH_RETRIES)
                      ? PROV_AUTH_NEEDED : PROV_ERROR;
        return false;
    }

    slot->state = code >= 500 ? PROV_DOWN : PROV_ERROR;
    Serial.printf("openai: usage error %d (http %d)\n", code, slot->last_http_code);
    return false;
}

static const CredField CREDS[] = {
    { "rt", "Refresh token from ~/.codex/auth.json (rt.1…)", true },
};

extern const ProviderDef OPENAI_DEF;
const ProviderDef OPENAI_DEF = {
    .id              = "openai",
    .name            = "Codex",
    .color           = 0x74AA9C,   // OpenAI teal
    .ca_pem          = CA_GTS_ROOT_R4,
    .poll_interval_s = 60,
    .creds           = CREDS,
    .cred_count      = 1,
    .fetch           = fetch,
};
