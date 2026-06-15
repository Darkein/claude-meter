#include "board.h"
#include "board_rev.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// AMOLED-1.8 also needs the XCA9554 IO expander up first — the display
// and touch controllers stay in reset until EXIO0..1 go HIGH.

static BoardRev g_rev = REV_SH8601_FT3168;

BoardRev board_rev(void) { return g_rev; }

// Probe an I2C address a few times — a single transaction can NAK on a noisy
// bus right after the controller leaves reset, and a missed CST816 ACK would
// silently pick the wrong display+touch driver (black screen).
static bool i2c_present(uint8_t addr) {
    for (int i = 0; i < 5; i++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) return true;
        delay(5);
    }
    return false;
}

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
    delay(10);  // let the touch controller exit reset before probing

    // Detect the panel revision by which touch controller answers. CST816
    // (0x15) ships on the CO5300 panel; FT3168 (0x38) on the original SH8601.
    // Probe both so an ambiguous bus is logged rather than silently defaulted.
    bool has_cst = i2c_present(CST816_ADDR);
    bool has_ft  = i2c_present(FT3168_ADDR);
    if (has_cst) {
        g_rev = REV_CO5300_CST816;
        Serial.println("Board revision: CO5300 + CST816");
    } else {
        g_rev = REV_SH8601_FT3168;
        Serial.println("Board revision: SH8601 + FT3168");
    }
    if (has_cst && has_ft)
        Serial.println("WARN: both touch controllers ACKed — defaulted to CO5300 + CST816");
    else if (!has_cst && !has_ft)
        Serial.println("WARN: no touch controller ACKed — defaulted to SH8601 + FT3168 (check I2C bus)");
}
