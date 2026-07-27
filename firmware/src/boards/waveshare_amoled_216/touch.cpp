#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            // This unit's touch frame is rotated 90° relative to the display
            // (verified empirically against on-screen targets): the controller's
            // X becomes screen Y and vice-versa, with one axis flipped. Remap so
            // coordinates line up with the on-screen layout.
            int16_t rawx = tx[0], rawy = ty[0];
            if (rawx < 0) rawx = 0; if (rawx >= LCD_WIDTH) rawx = LCD_WIDTH - 1;
            if (rawy < 0) rawy = 0;
            touch_x = (uint16_t)rawy;
            touch_y = (uint16_t)(LCD_WIDTH - 1 - rawx);
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
