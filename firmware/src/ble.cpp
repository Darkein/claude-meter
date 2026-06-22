#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <host/ble_gap.h>   // ble_gap_set_prefered_le_phy / ble_gap_set_data_len
#include "ota.h"
#include "version.h"
#include "hal/board_caps.h"

#define DEVICE_NAME "Claude Meter"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request
#define OTA_CHAR_UUID       "4c41555a-4465-7669-6365-000000000005"  // host streams firmware frames here
#define INFO_CHAR_UUID      "4c41555a-4465-7669-6365-000000000006"  // host reads board id + fw version

#define BLE_BUF_SIZE 512

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;
static NimBLECharacteristic* ota_char = nullptr;
static NimBLECharacteristic* info_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static volatile bool ota_finish_req = false;
static char mac_str[18];
static char info_json[160];

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet: flags + device name only. The host daemon
    // discovers the device by name ("Claude Meter"), so that's all we need.
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host daemon scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: connected from %s (active=%u)\n",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host daemon alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
};

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the daemon's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

// Firmware-update channel. Each write is one frame: a 1-byte opcode + payload.
//   0x01 BEGIN: u32 LE total size + 32-byte SHA-256 of the whole image
//   0x02 DATA : raw image bytes, in order (appended to the OTA partition)
//   0x03 END  : finalize — flagged here, committed on the main loop (the flash
//               commit + reboot must not run inside the BLE host task)
//   0x04 ABORT: cancel the in-progress transfer
// The opcode prefix on DATA disambiguates it from control frames, since image
// bytes can themselves start with 0x01/0x03/0x04.
class OtaCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string v = chr->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();
        switch (p[0]) {
        case 0x01:  // BEGIN
            if (n < 1 + 4 + 32) { ble_ota_notify_error(OTA_ERR_BEGIN); break; }
            {
                uint32_t size = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
                if (!ota_begin(size, p + 5)) {
                    ble_ota_notify_error(ota_last_error());
                } else if (server) {
                    // Crank the link for the transfer. All degrade gracefully if
                    // the central clamps/ignores them; relaxed on abort (a
                    // successful transfer reboots, which resets everything).
                    uint16_t ch = info.getConnHandle();
                    // Tight connection interval (7.5–15 ms): more events per second.
                    server->updateConnParams(ch, 6, 12, 0, 400);
                    // 2M PHY (BLE 5): double the raw symbol rate.
                    ble_gap_set_prefered_le_phy(ch, BLE_GAP_LE_PHY_2M_MASK,
                                                BLE_GAP_LE_PHY_2M_MASK, 0);
                    // Data Length Extension: 251-byte LL payload instead of 27, so
                    // a 511-byte ATT write is ~3 link-layer packets, not ~19.
                    ble_gap_set_data_len(ch, 251, 0x0848);
                }
            }
            break;
        case 0x02:  // DATA
            if (n > 1 && !ota_write(p + 1, n - 1))
                ble_ota_notify_error(ota_last_error());
            break;
        case 0x03:  // END
            ota_finish_req = true;
            break;
        case 0x04:  // ABORT
            ota_abort();
            if (server)  // back off to a relaxed interval
                server->updateConnParams(info.getConnHandle(), 24, 40, 0, 400);
            break;
        default:
            break;
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC
    // Ask for a large ATT MTU so OTA chunks are big (BlueZ negotiates up; macOS
    // caps ~185 and ignores this — the host adapts its chunk size either way).
    NimBLEDevice::setMTU(517);

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    // OTA firmware channel. WRITE (with response) gives free flow-control;
    // WRITE_NR is also allowed for a future faster path.
    ota_char = svc->createCharacteristic(
        OTA_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static OtaCallbacks otaCb;
    ota_char->setCallbacks(&otaCb);

    // Read-only board id + firmware version, so the OTA host can refuse a
    // wrong-board image and confirm the version after a reboot.
    info_char = svc->createCharacteristic(
        INFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    snprintf(info_json, sizeof(info_json),
        "{\"board\":\"%s\",\"fw\":\"%s\",\"git\":\"%s\"}",
        board_caps().id, FW_VERSION, FW_GIT);
    info_char->setValue((uint8_t*)info_json, strlen(info_json));

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_bonds(void) {
    return NimBLEDevice::getNumBonds() > 0;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

bool ble_ota_finish_requested(void) {
    if (!ota_finish_req) return false;
    ota_finish_req = false;
    return true;
}

void ble_ota_notify_done(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ota\":\"done\"}");
        tx_char->notify();
    }
}

void ble_ota_notify_error(int code) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        char buf[40];
        snprintf(buf, sizeof(buf), "{\"ota\":\"err\",\"c\":%d}", code);
        tx_char->setValue(buf);
        tx_char->notify();
    }
}
