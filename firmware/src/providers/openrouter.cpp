#include "provider.h"
#include "openrouter_parse.h"
#include "http.h"
#include "certs.h"
#include "secrets.h"
#include <Arduino.h>

// OpenRouter credits via GET /api/v1/key — a zero-cost status probe of the
// API key itself. Reports dollars used against an optional credit cap.
// openrouter.ai chains to the same GTS Root R4 as api.anthropic.com
// (verified 2026-07-26 via openssl s_client).

#define MAX_AUTH_RETRIES 3

static bool fetch(ProviderSlot* slot) {
    String auth = "Bearer " + secrets_get("openrouter", "tk");

    HttpRequest req = {};
    req.host          = "openrouter.ai";
    req.path          = "/api/v1/key";
    req.method        = "GET";
    req.auth          = auth.c_str();
    req.ca_pem        = CA_GTS_ROOT_R4;

    HttpResponse resp;
    http_perform(&req, &resp);
    slot->last_http_code = resp.code;

    if (resp.code == 200) {
        if (!openrouter_parse_ok(resp.body.c_str(), slot)) {
            slot->state = PROV_ERROR;
            Serial.println("openrouter: 200 but body unparseable");
            return false;
        }
        slot->state      = PROV_OK;
        slot->valid      = true;
        slot->auth_fails = 0;
        Serial.printf("openrouter: $%.2f used (limit %s)\n", slot->primary.value,
            slot->primary.limit > 0 ? String(slot->primary.limit, 2).c_str() : "none");
        return true;

    } else if (resp.code == 401 || resp.code == 403) {
        slot->auth_fails++;
        slot->state = (slot->auth_fails >= MAX_AUTH_RETRIES)
                      ? PROV_AUTH_NEEDED : PROV_ERROR;
        Serial.printf("openrouter: %d (#%d/%d)\n", resp.code,
            slot->auth_fails, MAX_AUTH_RETRIES);
        return false;

    } else if (resp.code >= 500) {
        slot->state = PROV_DOWN;
        Serial.printf("openrouter: %d — API down\n", resp.code);
        return false;

    } else {
        slot->state = PROV_ERROR;
        Serial.printf("openrouter: error %d\n", resp.code);
        return false;
    }
}

static const CredField CREDS[] = {
    { "tk", "API key (sk-or-v1-…)", true },
};

extern const ProviderDef OPENROUTER_DEF;
const ProviderDef OPENROUTER_DEF = {
    .id              = "openrouter",
    .name            = "OpenRouter",
    .color           = 0x6E7EF2,   // periwinkle
    .ca_pem          = CA_GTS_ROOT_R4,
    .poll_interval_s = 300,
    .creds           = CREDS,
    .cred_count      = 1,
    .fetch           = fetch,
};
