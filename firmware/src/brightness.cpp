#include "brightness.h"
#include "idle.h"
#include <Preferences.h>
#include <Arduino.h>

// Four-step ramp. The default (index 2) is 200 — identical to the prior
// hard-coded DISPLAY_DEFAULT_BRIGHTNESS, so cycling is purely additive.
static const uint8_t LEVELS[] = {64, 128, 200, 255};
#define LEVELS_COUNT (sizeof(LEVELS) / sizeof(LEVELS[0]))
#define DEFAULT_IDX  2

static uint8_t cur_idx = DEFAULT_IDX;

void brightness_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t saved_idx = prefs.getUChar("brt_idx", 0xFF);
    prefs.end();

    if (saved_idx < LEVELS_COUNT) cur_idx = saved_idx;
    idle_set_awake_brightness(LEVELS[cur_idx]);
    Serial.printf("Brightness init: level=%u (idx=%u)\n", LEVELS[cur_idx], cur_idx);
}

void brightness_cycle(void) {
    cur_idx = (cur_idx + 1) % LEVELS_COUNT;

    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("brt_idx", cur_idx);
    prefs.end();

    idle_set_awake_brightness(LEVELS[cur_idx]);
    Serial.printf("Brightness cycled: level=%u (idx=%u)\n", LEVELS[cur_idx], cur_idx);
}

void brightness_step(int delta) {
    int idx = (int)cur_idx + delta;
    if (idx < 0) idx = 0;
    if (idx >= (int)LEVELS_COUNT) idx = LEVELS_COUNT - 1;
    if ((uint8_t)idx == cur_idx) return;   // already at the end — nothing to do
    cur_idx = (uint8_t)idx;

    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("brt_idx", cur_idx);
    prefs.end();

    idle_set_awake_brightness(LEVELS[cur_idx]);
    Serial.printf("Brightness step: level=%u (idx=%u)\n", LEVELS[cur_idx], cur_idx);
}

uint8_t brightness_get(void) {
    return LEVELS[cur_idx];
}

uint8_t brightness_index(void) { return cur_idx; }
uint8_t brightness_count(void) { return LEVELS_COUNT; }
