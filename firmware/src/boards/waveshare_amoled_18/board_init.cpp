#include "board.h"
#include "board_rev.h"
#include "board_check.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// AMOLED-1.8 also needs the XCA9554 IO expander up first — the display
// and touch controllers stay in reset until EXIO0..1 go HIGH.

static BoardRev g_rev = REV_SH8601_FT3168;

BoardRev board_rev(void) { return g_rev; }

static bool i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
    delay(10);  // let the touch controller exit reset before probing

    // Detect the panel revision by which touch controller answers. CST816
    // (0x15) ships on the CO5300 panel; FT3168 (0x38) on the original SH8601.
    //
    // The FT3168 branch is also the fall-through when nothing answers at all,
    // so it cannot double as a presence test — board_check_touch() below is
    // what distinguishes "the other revision" from "not this board".
    if (i2c_present(CST816_ADDR)) {
        g_rev = REV_CO5300_CST816;
        Serial.println("Board revision: CO5300 + CST816");
    } else {
        g_rev = REV_SH8601_FT3168;
        Serial.println("Board revision: SH8601 + FT3168");
    }

    static const uint8_t expect[] = {FT3168_ADDR, CST816_ADDR};
    board_check_touch(expect, 2, BOARD_NAME);
}
