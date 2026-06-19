#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
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
    // C6 2.16 panel mapping. The original values (swap=true, mirrorX=true)
    // were calibrated to the old display orientation that force-wrote MADCTL
    // 0x30 (MV transpose + ML). The display now runs at the CO5300 class
    // default (MADCTL 0x00 — USB-port-on-the-side orientation), so the touch
    // mapping is re-derived to match. SensorLib applies swap then mirror
    // (TouchDrvInterface::updateXY); tap-tested on C6 hardware, the raw
    // CST9217 coordinates map straight through in this orientation — no swap,
    // no mirror.
    touch.setSwapXY(false);
    touch.setMirrorXY(false, false);
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
            // The display rotates its content by the IMU quadrant
            // (display_hal_draw_bitmap); touch arrives in raw panel coordinates,
            // so undo the same rotation to land back in LVGL's logical frame.
            // Inverse of rotate_strip's point map on the square 480x480 panel.
            int rx = tx[0], ry = ty[0];
            const int S = LCD_WIDTH; // == LCD_HEIGHT (square)
            int lx, ly;
            switch (imu_hal_rotation_quadrant()) {
            case 1:  lx = ry;         ly = S - 1 - rx; break;
            case 2:  lx = S - 1 - rx; ly = S - 1 - ry; break;
            case 3:  lx = S - 1 - ry; ly = rx;         break;
            default: lx = rx;         ly = ry;         break;
            }
            touch_pressed = true;
            touch_x = (uint16_t)lx;
            touch_y = (uint16_t)ly;
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
