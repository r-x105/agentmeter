#pragma once
#include <stdbool.h>

// Always-on LAN configuration page. Starts lazily once the device is
// STA-connected (http://agentmeter.local or the device IP) and serves a
// single form: per-provider credential fields (write-only; stored values
// are shown masked and never echoed back), enable toggles, and the
// primary-provider picker. Saves apply live — no reboot.
//
// Threat model (see README): plain HTTP on the trusted LAN, no page auth in
// v1 — anyone on the network can change device settings.

void config_server_tick(void);      // call every loop(); starts/handles clients
bool config_server_running(void);

// Rotating 8-char device code (unambiguous alphabet). Shown on the device's
// Wi-Fi screen; /save rejects requests without it, so someone on the LAN
// can't change the config unless they can also see the panel. Rotates every
// 10 minutes and immediately after each successful save.
const char* config_server_code(void);
void        config_server_rotate_code(void);   // manual rotation (device UI button)
