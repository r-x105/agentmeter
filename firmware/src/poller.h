#pragma once
#include "providers/provider.h"

// Wi-Fi connection state machine + the single sequential provider-poll task.
// Generalization of the old wifi_poller: Wi-Fi-level state stays global (and
// keeps the captive-portal fallback contract with main.cpp); per-provider
// results live in the registry slots.

typedef enum {
    POLLER_INIT = 0,
    POLLER_NO_CREDS,     // no Wi-Fi SSID/pass stored
    POLLER_CONNECTING,
    POLLER_WIFI_FAIL,    // connect timed out
    POLLER_CONNECTED,
} poller_wifi_status_t;

void poller_init(void);
void poller_tick(void);
bool poller_is_connected(void);
poller_wifi_status_t poller_wifi_status(void);
int  poller_wifi_fail_count(void);   // consecutive connect failures (portal fallback)
void poller_stop(void);              // drain in-flight HTTP, disconnect (portal handoff)

// Progress toward slot i's next poll, 0..100 (100 = due/overdue).
int  poller_slot_progress(int i);
