#pragma once

void captive_portal_init(void);   // auto-starts if no credentials stored
void captive_portal_start(void);  // start immediately (call poller_stop first)
void captive_portal_tick(void);
bool captive_portal_is_active(void);
