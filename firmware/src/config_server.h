#pragma once
#include <stdbool.h>

// Always-on LAN configuration page. Starts lazily once the device is
// STA-connected (http://agentmeter.local or the device IP).
//
// The page is locked until the visitor enters the rotating device code shown
// on the panel. Unlocking mints a session cookie (HttpOnly, SameSite=Strict,
// 30 min sliding); every mutating route requires it. Only then does the form
// appear: per-provider credential fields (write-only; stored values are shown
// masked and never echoed back), enable toggles, and the primary-provider
// picker. Saves apply live — no reboot.
//
// Threat model: plain HTTP on the trusted LAN. Unlocking requires seeing the
// panel, so a passer-by on the network cannot change settings — but the code
// and session cookie both travel in clear, so anyone able to sniff the LAN
// within the session's lifetime can ride it. /status stays open: it is
// read-only and carries no secrets. Don't put the device on a network you
// don't trust.

void config_server_tick(void);      // call every loop(); starts/handles clients
bool config_server_running(void);

// Rotating 8-char device code (unambiguous alphabet). Shown on the device's
// Wi-Fi screen; /save rejects requests without it, so someone on the LAN
// can't change the config unless they can also see the panel. Rotates every
// 10 minutes and immediately after each successful save.
const char* config_server_code(void);
void        config_server_rotate_code(void);   // manual rotation (device UI button)
