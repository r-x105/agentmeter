#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_SETTINGS,
    SCREEN_WIFI,
    SCREEN_COUNT,
};

typedef enum {
    UI_STATUS_NONE,
    UI_STATUS_INFO,
    UI_STATUS_WARN,
    UI_STATUS_ERROR,
} ui_status_level_t;

void ui_init(void);
void ui_update_providers(void);          // pull slot snapshots, refresh all tiles
void ui_rebuild_provider_tiles(void);    // call after the enabled-provider set changes
// Jump the usage tileview; QA + debugging. col 0 = overview (paginated
// vertically by `page`), col 1..N = provider cards (always page 0).
void ui_show_tile(int col, int page = 0);
void ui_set_status(ui_status_level_t level, const char* msg);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);   // PWR button: Usage -> Settings -> Wi-Fi -> Usage
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
void ui_set_refresh_progress(int pct);   // 0..100 ring to next Anthropic poll
void ui_update_wifi_creds(bool portal_active);
bool ui_hotspot_requested(void);
bool ui_reboot_requested(void);   // Wi-Fi "Back" while the hotspot is active
void ui_set_nav_locked(bool locked);
