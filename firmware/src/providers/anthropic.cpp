#include "provider.h"
#include "anthropic_parse.h"
#include "http.h"
#include "certs.h"
#include "secrets.h"
#include <Arduino.h>
#include <time.h>

// Anthropic / Claude usage via the anthropic-ratelimit-unified-* response
// headers of a minimal 1-token POST /v1/messages. We stay on /v1/messages
// (not the cheaper /api/oauth/usage) because the long-lived `claude setup
// token` carries only inference scope and 403s there.
//
// The provider owns 401 counting: a 401 burst can be an auth-server wobble,
// so AUTH_NEEDED (which stops polling) only fires after 3 consecutive 401s.

#define MAX_AUTH_RETRIES 3

static const char* COLLECT[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-status",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-reset",
};
static const char* EXTRA[] = { "anthropic-version", "2023-06-01" };

static bool fetch(ProviderSlot* slot) {
    String auth = "Bearer " + secrets_get("anthropic", "tk");

    HttpRequest req = {};
    req.host          = "api.anthropic.com";
    req.path          = "/v1/messages";
    req.method        = "POST";
    req.body          = "{\"model\":\"claude-haiku-4-5-20251001\","
                        "\"max_tokens\":1,"
                        "\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}";
    req.content_type  = "application/json";
    req.auth          = auth.c_str();
    req.extra_headers = EXTRA;
    req.extra_count   = 1;
    req.collect       = COLLECT;
    req.collect_count = 5;
    req.ca_pem        = CA_GTS_ROOT_R4;

    HttpResponse resp;
    http_perform(&req, &resp);
    slot->last_http_code = resp.code;
    long now_epoch = (long)time(nullptr);

    if (resp.code == 200) {
        if (!anthropic_parse_ok(resp.headers[0].c_str(), resp.headers[1].c_str(),
                                resp.headers[2].c_str(), resp.headers[3].c_str(),
                                resp.headers[4].c_str(), now_epoch, slot)) {
            slot->state = PROV_ERROR;
            Serial.println("anthropic: 200 but headers unparseable");
            return false;
        }
        slot->state      = PROV_OK;
        slot->valid      = true;
        slot->auth_fails = 0;
        Serial.printf("anthropic: s=%.1f%% w=%.1f%% status=%s\n",
            slot->primary.value, slot->secondary.value, slot->status);
        return true;

    } else if (resp.code == 401) {
        slot->auth_fails++;
        slot->state = (slot->auth_fails >= MAX_AUTH_RETRIES)
                      ? PROV_AUTH_NEEDED : PROV_ERROR;
        Serial.printf("anthropic: 401 (#%d/%d)%s\n", slot->auth_fails,
            MAX_AUTH_RETRIES,
            slot->state == PROV_AUTH_NEEDED ? " — polling stopped" : "");
        return false;

    } else if (resp.code == 429) {
        // A once-a-minute 1-token poll can't trip a burst limit, so a 429
        // means the account usage window is exhausted. The unified-* headers
        // come back on 429s too — keep the real weekly figure while forcing
        // the session bar full.
        anthropic_apply_limited(resp.headers[1].c_str(), resp.headers[3].c_str(),
                                now_epoch, slot);
        slot->state = PROV_LIMITED;
        slot->valid = true;
        Serial.println("anthropic: 429 — limit reached");
        return false;  // still a failure for backoff purposes

    } else if (resp.code >= 500) {
        slot->state = PROV_DOWN;
        Serial.printf("anthropic: %d — API down\n", resp.code);
        return false;

    } else {
        slot->state = PROV_ERROR;
        Serial.printf("anthropic: error %d\n", resp.code);
        return false;
    }
}

static const CredField CREDS[] = {
    { "tk", "Setup token (sk-ant-oat01-…)", true },
};

extern const ProviderDef ANTHROPIC_DEF;
const ProviderDef ANTHROPIC_DEF = {
    .id              = "anthropic",
    .name            = "Claude",
    .color           = 0xCC785C,   // Anthropic clay
    .ca_pem          = CA_GTS_ROOT_R4,
    .poll_interval_s = 60,
    .creds           = CREDS,
    .cred_count      = 1,
    .fetch           = fetch,
};
