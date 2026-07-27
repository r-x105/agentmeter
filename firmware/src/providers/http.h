#pragma once
#include <Arduino.h>

// Shared TLS + HTTP helper for provider fetch() implementations. One TLS
// session alive at a time (the poller is strictly sequential), so RAM stays
// flat as providers are added. Poller task only — not thread-safe.

#define PROVIDER_HTTP_TIMEOUT_MS 10000
#define HTTP_MAX_COLLECT 8

typedef struct {
    const char*        host;           // "api.anthropic.com"
    const char*        path;           // "/v1/messages"
    const char*        method;         // "GET" or "POST"
    const char*        body;           // nullptr for GET
    const char*        content_type;   // nullptr = none
    const char*        auth;           // full Authorization value ("Bearer …") or nullptr
    const char* const* extra_headers;  // flat k,v pairs: {"anthropic-version","2023-06-01"}
    int                extra_count;    // number of PAIRS
    const char* const* collect;        // response headers to capture
    int                collect_count;  // <= HTTP_MAX_COLLECT
    const char*        ca_pem;         // trust anchor from certs.h
} HttpRequest;

typedef struct {
    int    code;                       // HTTP status; negative = transport/TLS failure
    String body;                       // capped at 4 KB (providers return small payloads)
    String headers[HTTP_MAX_COLLECT];  // values in `collect` order; "" if absent
} HttpResponse;

// Routes large mbedTLS allocations to PSRAM (S3). Call once at boot.
void provider_http_init(void);

// Returns false only on transport-level failure (resp->code < 0);
// any HTTP status, including 4xx/5xx, returns true with the code set.
bool http_perform(const HttpRequest* req, HttpResponse* resp);
