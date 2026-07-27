#include "poller.h"
#include "providers/http.h"
#include "provisioning.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <time.h>

#define CONNECT_TIMEOUT_MS  15000
#define RETRY_INTERVAL_MS   30000
// Per-slot exponential backoff on consecutive poll failures: base interval
// doubled per failure, capped. Stops re-attempting the heavy TLS handshake
// every cycle during an outage (SRAM is tight with NimBLE + HTTPS).
#define MAX_BACKOFF_MS      900000   // 15 min

enum wifi_state_t {
    WIFI_ST_IDLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
};

static wifi_state_t         s_state      = WIFI_ST_IDLE;
static uint32_t             s_state_ts   = 0;
static poller_wifi_status_t s_status     = POLLER_INIT;
static int                  s_fail_count = 0;   // Wi-Fi connect fails (portal fallback)

static volatile bool  s_poll_busy    = false;
static volatile bool  s_stop_polling = false;
static volatile int   s_active_slot  = -1;      // slot the task is fetching
static TaskHandle_t   s_poll_task    = nullptr;
static StackType_t*   s_poll_stack   = nullptr;
static StaticTask_t   s_poll_tcb;

static void begin_connect() {
    String ssid = provisioning_get_ssid();
    WiFi.begin(ssid.c_str(), provisioning_get_pass().c_str());
    s_state    = WIFI_ST_CONNECTING;
    s_state_ts = millis();
    s_status   = POLLER_CONNECTING;
    Serial.printf("wifi: connecting to \"%s\"...\n", ssid.c_str());
}

// Effective interval for a slot right now: base × 2^fail_count, capped.
static uint32_t slot_interval_ms(const ProviderSlot* s) {
    uint32_t base  = (uint32_t)s->def->poll_interval_s * 1000;
    uint32_t shift = s->fail_count < 4 ? s->fail_count : 4;
    uint64_t iv    = (uint64_t)base << shift;
    return iv > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : (uint32_t)iv;
}

// Runs on the poll task: fetch into a local copy so the registry mutex is
// never held across a 10 s HTTP request, then commit.
static void do_poll(int idx) {
    ProviderSlot local;
    provider_snapshot(idx, &local);

    Serial.printf("poll: %s start (heap int=%lu/%lu psram=%lu)\n", local.def->id,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool ok = local.def->fetch(&local);

    uint32_t now = millis();
    if (ok) {
        local.fail_count = 0;
        local.last_ok_ms = now;
    } else if (local.fail_count < 8) {
        local.fail_count++;
    }
    local.last_poll_ms = now;
    local.next_poll_ms = now + slot_interval_ms(&local);

    provider_commit(idx, &local);
    Serial.printf("poll: %s done (%s, next in %lus)\n", local.def->id,
        ok ? "ok" : "fail", (unsigned long)(slot_interval_ms(&local) / 1000));
}

static void poll_task_fn(void* param) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int idx = s_active_slot;
        if (idx >= 0) do_poll(idx);
        s_poll_busy = false;
    }
}

void poller_init(void) {
    provider_http_init();

    // Poll task stack in PSRAM where available (EXT_RAM_BSS_ATTR does not
    // work with this pioarduino build — runtime alloc is required). Two hard
    // constraints meet here, verified on hardware 2026-07-26:
    //  1. The stack cannot be internal SRAM: +12 KB tips lwIP/TLS into
    //     allocation failures and every DNS lookup / handshake errors out.
    //  2. A PSRAM-stack task cannot touch NVS: flash writes disable the
    //     caches and trip esp_task_stack_is_sane → instant boot-loop.
    // Resolution: stack stays in PSRAM and providers NEVER write NVS from
    // fetch() — secrets_set() only updates the RAM cache and queues the
    // write; the main loop (internal stack) calls secrets_flush() each tick.
    s_poll_stack = (StackType_t*)heap_caps_malloc(12288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_poll_stack) {
        Serial.println("poll: PSRAM stack alloc failed, falling back to SRAM");
        s_poll_stack = (StackType_t*)malloc(12288);
    }
    s_poll_task = xTaskCreateStaticPinnedToCore(
        poll_task_fn, "prov_poll",
        12288 / sizeof(StackType_t),
        nullptr, 1,
        s_poll_stack, &s_poll_tcb, 0
    );

    if (!provisioning_has_wifi()) {
        s_status = POLLER_NO_CREDS;
        Serial.println("wifi: no credentials, skipping");
        return;
    }
    begin_connect();
}

void poller_tick(void) {
    uint32_t now = millis();

    switch (s_state) {
        case WIFI_ST_IDLE:
            break;

        case WIFI_ST_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                s_state      = WIFI_ST_CONNECTED;
                s_status     = POLLER_CONNECTED;
                s_fail_count = 0;
                Serial.printf("wifi: connected, IP=%s\n",
                    WiFi.localIP().toString().c_str());
                configTime(0, 0, "pool.ntp.org");
                // Fresh link → poll every enabled slot promptly, clear backoff.
                for (int i = 0; i < provider_count(); i++) {
                    ProviderSlot s;
                    provider_snapshot(i, &s);
                    s.fail_count   = 0;
                    s.next_poll_ms = now;
                    provider_commit(i, &s);
                }
                (void)provider_take_new_data();  // don't wake the UI for this
            } else if (now - s_state_ts >= CONNECT_TIMEOUT_MS) {
                WiFi.disconnect();
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                s_status   = POLLER_WIFI_FAIL;
                s_fail_count++;
                Serial.printf("wifi: timeout (status=%d), fail #%d, retry in %us\n",
                    WiFi.status(), s_fail_count, RETRY_INTERVAL_MS / 1000);
            }
            break;

        case WIFI_ST_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                s_status   = POLLER_WIFI_FAIL;
                Serial.println("wifi: lost connection, retry in 30s");
            } else if (!s_poll_busy && !s_stop_polling) {
                // Dispatch the first due slot (sequential — one TLS session
                // alive at a time). Round-robin fairness comes free: a slot
                // that just polled has the farthest next_poll_ms.
                for (int i = 0; i < provider_count(); i++) {
                    ProviderSlot s;
                    provider_snapshot(i, &s);
                    if (!s.enabled || !s.configured) continue;
                    if (s.state == PROV_AUTH_NEEDED) continue;  // stopped until re-config
                    if (s.next_poll_ms != 0 && now < s.next_poll_ms) continue;
                    s_active_slot = i;
                    s_poll_busy   = true;
                    xTaskNotifyGive(s_poll_task);
                    break;
                }
            }
            break;

        case WIFI_ST_FAILED:
            if (now - s_state_ts >= RETRY_INTERVAL_MS) {
                begin_connect();
            }
            break;
    }
}

bool poller_is_connected(void)               { return s_state == WIFI_ST_CONNECTED; }
poller_wifi_status_t poller_wifi_status(void){ return s_status; }
int  poller_wifi_fail_count(void)            { return s_fail_count; }

// Cheap cross-core reads — a benign race that at worst nudges the ring.
int poller_slot_progress(int i) {
    ProviderSlot s;
    provider_snapshot(i, &s);
    if (s.last_poll_ms == 0) return 0;
    uint32_t interval = s.next_poll_ms - s.last_poll_ms;
    if (interval == 0) return 100;
    uint32_t elapsed = millis() - s.last_poll_ms;
    if (elapsed >= interval) return 100;
    return (int)((uint64_t)elapsed * 100 / interval);
}

void poller_stop(void) {
    s_stop_polling = true;
    s_state        = WIFI_ST_IDLE;
    // Wait for any in-flight HTTP request to finish before disconnecting.
    // The caller (main.cpp) immediately calls captive_portal_start() →
    // WiFi.mode(WIFI_AP), which tears down the station interface; racing
    // that against an active TLS socket can assert in the WiFi driver.
    for (uint32_t t0 = millis(); s_poll_busy && millis() - t0 < PROVIDER_HTTP_TIMEOUT_MS + 500;)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_status     = POLLER_NO_CREDS;
    s_fail_count = 0;
    WiFi.disconnect();
    Serial.println("wifi: stopped");
}
