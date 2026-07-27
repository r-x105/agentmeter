#pragma once
#include <stdint.h>

// Guards against flashing one board's firmware onto another board in the family.
//
// This failure mode is nastier than it sounds. The boards share their I2C
// peripherals — same PMU, same IMU, same codec, same SDA/SCL pins — so a
// wrong-board build boots, connects to Wi-Fi, polls providers and prints
// "Dashboard ready". Only the QSPI display pins and the touch address differ,
// so what the owner sees is a black screen and dead touch: symptoms that read
// as broken hardware, not as the wrong binary. The check below turns that into
// one unmissable line.
//
// Each board calls this from its own board_init() with the touch address(es)
// its build targets, right after Wire.begin().
void board_check_touch(const uint8_t* expect, int n, const char* board_name);

// True once board_check_touch() has found a mismatch.
bool board_check_failed(void);

// Re-prints the banner periodically while mismatched. A wrong-board device has
// no working display, so serial is the only channel left — and a banner that
// scrolled past before the owner attached a monitor helps nobody.
void board_check_tick(void);
