#include "http.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/platform.h"
#include <esp_heap_caps.h>

// Large enough for the OpenAI refresh response (~4.5 KB with its id_token).
#define BODY_CAP 8192

// Route mbedTLS allocations (SSL record buffers, handshake state, x509
// parsing) to PSRAM, leaving internal SRAM for lwIP pbufs and the Wi-Fi
// driver's dynamic RX buffers — when those can't allocate, every connection
// dies with -2/-5/-54 regardless of TLS state. Threshold lowered from 512 to
// 64 bytes (2026-07-26): with the config server + multi-provider state the
// internal heap sits low enough that even mid-size TLS allocs must go
// external. On PSRAM-free boards the SPIRAM alloc fails and we fall back to
// plain calloc — same behavior as before, just under more pressure.
static void* tls_calloc_psram(size_t n, size_t size) {
    void* ptr = nullptr;
    if (n * size >= 64) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) ptr = calloc(n, size);
    return ptr;
}

void provider_http_init(void) {
    mbedtls_platform_set_calloc_free(tls_calloc_psram, free);
}

bool http_perform(const HttpRequest* req, HttpResponse* resp) {
    resp->code = -1;
    resp->body = "";
    for (int i = 0; i < HTTP_MAX_COLLECT; i++) resp->headers[i] = "";

    WiFiClientSecure tls;
    tls.setCACert(req->ca_pem);

    HTTPClient http;
    http.setTimeout(PROVIDER_HTTP_TIMEOUT_MS);
    if (req->collect_count > 0) {
        // HTTPClient wants a non-const array pointer; it only reads from it.
        http.collectHeaders(const_cast<const char**>(req->collect), req->collect_count);
    }

    String url = "https://" + String(req->host) + String(req->path);
    if (!http.begin(tls, url)) {
        Serial.printf("http: begin failed (%s)\n", req->host);
        return false;
    }

    if (req->auth)         http.addHeader("Authorization", req->auth);
    if (req->content_type) http.addHeader("Content-Type", req->content_type);
    for (int i = 0; i < req->extra_count; i++) {
        http.addHeader(req->extra_headers[i * 2], req->extra_headers[i * 2 + 1]);
    }

    int code;
    if (strcmp(req->method, "POST") == 0) {
        code = http.POST(req->body ? String(req->body) : String());
    } else {
        code = http.GET();
    }
    resp->code = code;

    if (code > 0) {
        for (int i = 0; i < req->collect_count && i < HTTP_MAX_COLLECT; i++) {
            resp->headers[i] = http.header(req->collect[i]);
        }
        resp->body = http.getString();
        if (resp->body.length() > BODY_CAP) resp->body = resp->body.substring(0, BODY_CAP);
    }

    http.end();
    return code > 0;
}
