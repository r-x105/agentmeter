#include "board.h"
#include "board_check.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the shared I2C bus. AMOLED-2.16 has no IO expander, so this is
// all the early init needed before display/touch/power/imu HAL calls.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    // This build drives the 2.16's QSPI pins; if the CST9220 isn't there, it
    // isn't this board and nothing below will reach the panel.
    static const uint8_t expect[] = {CST9220_ADDR};
    board_check_touch(expect, 1, BOARD_NAME);
}
