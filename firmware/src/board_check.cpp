#include "board_check.h"
#include <Arduino.h>
#include <Wire.h>

// The touch controller is the cheapest board fingerprint available: it is the
// one I2C device whose address differs across the family. The PMU (0x34), IMU
// (0x6B) and codec (0x18) answer identically on every board and so prove
// nothing about which one this is.
struct BoardSig {
    uint8_t     addr;
    const char* board;
    const char* env;
};

static const BoardSig SIGS[] = {
    {0x5A, "Waveshare AMOLED 2.16", "waveshare_amoled_216"},
    {0x38, "Waveshare AMOLED 1.8",  "waveshare_amoled_18"},
    {0x15, "Waveshare AMOLED 1.8",  "waveshare_amoled_18"},
};
#define SIG_COUNT ((int)(sizeof(SIGS) / sizeof(SIGS[0])))

static bool        g_failed     = false;
static const char* g_built_for  = nullptr;
static const char* g_detected   = nullptr;
static const char* g_env        = nullptr;
static uint32_t    g_last_print = 0;

#define REPRINT_INTERVAL_MS 15000UL

static bool present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void print_banner(void) {
    Serial.println();
    Serial.println("========================================================");
    Serial.println("  WRONG FIRMWARE FOR THIS BOARD");
    Serial.printf ("  built for : %s\n", g_built_for ? g_built_for : "?");
    if (g_detected) {
        Serial.printf("  detected  : %s\n", g_detected);
        Serial.printf("  reflash   : pio run -e %s -t upload\n", g_env);
    } else {
        Serial.println("  detected  : no known touch controller on the I2C bus");
    }
    Serial.println("  Until then the display stays dark and touch is dead.");
    Serial.println("========================================================");
    Serial.println();
}

void board_check_touch(const uint8_t* expect, int n, const char* board_name) {
    for (int i = 0; i < n; i++)
        if (present(expect[i])) return;      // the expected controller answered

    // Nothing this build knows about is on the bus. Report what actually is,
    // so the message names the env to flash instead of just saying "wrong".
    g_failed    = true;
    g_built_for = board_name;
    for (int i = 0; i < SIG_COUNT; i++) {
        if (present(SIGS[i].addr)) {
            g_detected = SIGS[i].board;
            g_env      = SIGS[i].env;
            break;
        }
    }
    print_banner();
    g_last_print = millis();
}

bool board_check_failed(void) { return g_failed; }

void board_check_tick(void) {
    if (!g_failed) return;
    uint32_t now = millis();
    if (now - g_last_print < REPRINT_INTERVAL_MS) return;
    g_last_print = now;
    print_banner();
}
