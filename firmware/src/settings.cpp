#include "settings.h"
#include <Preferences.h>
#include <Arduino.h>

static bool s_status_text = true;   // default: show the whimsical status line
static bool s_clock_enabled = false;
static int  s_clock_offset  = 0;    // minutes from UTC
static bool s_clock_24h     = true;
static bool s_clock_seconds = false;

#define OFFSET_MIN (-12 * 60)       // UTC-12:00
#define OFFSET_MAX ( 14 * 60)       // UTC+14:00

void settings_init(void) {
    Preferences p;
    p.begin("clawdmeter", true);
    s_status_text   = p.getBool("status_txt", true);
    s_clock_enabled = p.getBool("clk_en", false);
    s_clock_offset  = p.getInt("clk_off", 0);
    s_clock_24h     = p.getBool("clk_24", true);
    s_clock_seconds = p.getBool("clk_sec", false);
    p.end();
    Serial.printf("Settings init: status_text=%d clock=%d off=%d 24h=%d\n",
        s_status_text, s_clock_enabled, s_clock_offset, s_clock_24h);
}

bool settings_status_text(void) { return s_status_text; }

void settings_set_status_text(bool on) {
    s_status_text = on;
    Preferences p;
    p.begin("clawdmeter", false);
    p.putBool("status_txt", on);
    p.end();
    Serial.printf("Settings: status_text=%d\n", on ? 1 : 0);
}

bool settings_clock_enabled(void) { return s_clock_enabled; }
int  settings_clock_offset_min(void) { return s_clock_offset; }
bool settings_clock_24h(void) { return s_clock_24h; }

void settings_set_clock_enabled(bool on) {
    s_clock_enabled = on;
    Preferences p; p.begin("clawdmeter", false); p.putBool("clk_en", on); p.end();
}

void settings_adjust_clock_offset(int delta) {
    s_clock_offset += delta;
    if (s_clock_offset < OFFSET_MIN) s_clock_offset = OFFSET_MIN;
    if (s_clock_offset > OFFSET_MAX) s_clock_offset = OFFSET_MAX;
    Preferences p; p.begin("clawdmeter", false); p.putInt("clk_off", s_clock_offset); p.end();
}

void settings_set_clock_24h(bool on) {
    s_clock_24h = on;
    Preferences p; p.begin("clawdmeter", false); p.putBool("clk_24", on); p.end();
}

bool settings_clock_seconds(void) { return s_clock_seconds; }
void settings_set_clock_seconds(bool on) {
    s_clock_seconds = on;
    Preferences p; p.begin("clawdmeter", false); p.putBool("clk_sec", on); p.end();
}
