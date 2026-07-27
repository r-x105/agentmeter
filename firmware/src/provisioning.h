#pragma once
#include <Arduino.h>

void provisioning_init(void);
void provisioning_handle_cmd(const char* cmd);

bool     provisioning_has_wifi(void);
bool     provisioning_has_token(void);
String   provisioning_get_ssid(void);
String   provisioning_get_pass(void);
String   provisioning_get_token(void);

// Write ssid + pass (and optionally token) in a single NVS transaction.
// Pass nullptr for token to leave the stored token unchanged.
void provisioning_save_wifi(const char* ssid, const char* pass, const char* token);
