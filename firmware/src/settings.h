#pragma once
#include <stdbool.h>

// User-facing preferences edited from the Settings screen and persisted to NVS
// (namespace "clawdmeter"). Brightness has its own module (brightness.{h,cpp});
// this holds the rest. Load once at boot via settings_init() before ui_init().

void settings_init(void);                  // load persisted values from NVS

bool settings_status_text(void);           // whimsical bottom status line on/off
void settings_set_status_text(bool on);    // set + persist

// Usage-screen clock (device NTP time + a fixed UTC offset).
bool settings_clock_enabled(void);
void settings_set_clock_enabled(bool on);
int  settings_clock_offset_min(void);          // minutes from UTC (e.g. +5:30 = 330)
void settings_adjust_clock_offset(int delta);  // step & persist, clamped
bool settings_clock_24h(void);                 // true = 24h, false = 12h
void settings_set_clock_24h(bool on);
bool settings_clock_seconds(void);             // show seconds (HH:MM:SS)
void settings_set_clock_seconds(bool on);
