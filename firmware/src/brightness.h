#pragma once
#include <stdint.h>

// User-controlled display brightness, persisted to NVS. The middle (PWR)
// button short-press cycles through the levels via brightness_cycle().
// idle owns the actual panel brightness, so this routes the chosen level
// through idle_set_awake_brightness().
void    brightness_init(void);    // load saved level from NVS and apply
void    brightness_cycle(void);   // advance to next level (wraps), save, apply
void    brightness_step(int delta);  // +1 / -1 through the levels (clamped, no wrap)
uint8_t brightness_get(void);     // current PWM level (0..255)
uint8_t brightness_index(void);   // current level index (0..count-1)
uint8_t brightness_count(void);   // number of discrete levels
