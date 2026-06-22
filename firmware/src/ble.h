#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

// --- OTA (firmware update over BLE) ---
// Set true once an END frame arrives; reading it clears the flag. The main loop
// owns finalization (flash commit + reboot) so it never runs inside the BLE
// host-task callback. See ota.h.
bool ble_ota_finish_requested(void);
// Report the terminal OTA result to the host on the TX characteristic.
void ble_ota_notify_done(void);
void ble_ota_notify_error(int code);
