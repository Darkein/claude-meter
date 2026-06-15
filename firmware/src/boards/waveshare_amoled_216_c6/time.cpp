#include "../../hal/time_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorPCF85063.hpp>

// PCF85063 RTC on the shared I2C bus (SDA=8, SCL=7, already up via board_init).
// Keeps wall-clock time across reboots and daemon outages; the shared clock
// module seeds itself from here at boot, before the first BLE poll arrives.
//
// We store the wall-clock epoch the daemon sends *as-is* (it's already local
// wall time — see daemon's "t" field), so we use gmtime/timegm here: no extra
// timezone offset is applied on the device.

static SensorPCF85063 rtc;
static bool rtc_ok = false;

void time_hal_init(void) {
    rtc_ok = rtc.init(Wire, IIC_SDA, IIC_SCL, PCF85063_ADDR);
    Serial.printf("PCF85063 RTC init %s\n", rtc_ok ? "OK" : "FAILED");
}

bool time_hal_now(struct tm *out) {
    if (!rtc_ok || !out)
        return false;
    RTC_DateTime dt = rtc.getDateTime();
    // available=false means the oscillator-stop flag is set: the RTC lost power
    // and was never reset, so its time is meaningless. Reject years < 2024 too
    // (a freshly-powered, never-set chip reads back 2000).
    if (!dt.available || dt.year < 2024)
        return false;
    rtc.getDateTime(out);
    return true;
}

void time_hal_set_epoch(uint32_t e) {
    if (!rtc_ok)
        return;
    time_t t = (time_t)e;
    struct tm tmv;
    gmtime_r(&t, &tmv);  // epoch is already local wall time; no offset re-applied
    rtc.setDateTime((uint16_t)(tmv.tm_year + 1900), (uint8_t)(tmv.tm_mon + 1),
                    (uint8_t)tmv.tm_mday, (uint8_t)tmv.tm_hour,
                    (uint8_t)tmv.tm_min, (uint8_t)tmv.tm_sec);
}
