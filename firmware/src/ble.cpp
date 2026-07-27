#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#define DEVICE_NAME "Clawdmeter"

// HID keyboard report descriptor (standard 6-KRO boot-protocol-compatible).
// Includes the LED output report (Num/Caps/Scroll Lock indicators) — without
// it macOS's Keyboard Setup Assistant flags the device as "unidentifiable"
// because the descriptor doesn't look like a complete keyboard.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    // LED output report — required for macOS to treat this as a full keyboard.
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Constant) - LED report padding
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection
};

static NimBLEServer*        server    = nullptr;
static NimBLEHIDDevice*     hid_dev   = nullptr;
static NimBLECharacteristic* input_kbd = nullptr;

static ble_state_t state         = BLE_STATE_INIT;
static bool        need_advertise = false;
static char        mac_str[18];

// --- Single-owner lock -----------------------------------------------------
//
// The board is a BLE peripheral that any central in range could connect to and
// write usage data to. To stop the display rotating to another machine's
// account, it locks to ONE owner: the identity address of the machine it is
// bonded to, persisted in NVS. Only that owner (over a bonded+encrypted link)
// may write usage data; a second machine that pairs is rejected so the board
// stays paired to a single machine. The hold-power bond-clear gesture resets
// the owner so the board can be handed to a different machine.
static Preferences prefs;
static char owner_addr[18] = {0};   // owner identity address, e.g. "aa:bb:cc:dd:ee:ff"
static bool owner_set = false;
static const char* ZERO_ADDR = "00:00:00:00:00:00";

static void save_owner() {
    prefs.begin("clawd", false);
    prefs.putString("owner", owner_addr);
    prefs.end();
}

static void clear_owner() {
    owner_set = false;
    owner_addr[0] = '\0';
    prefs.begin("clawd", false);
    prefs.remove("owner");
    prefs.end();
}

static void load_owner() {
    prefs.begin("clawd", true);
    String o = prefs.getString("owner", "");
    prefs.end();
    if (o.length() == 17) {  // "aa:bb:cc:dd:ee:ff"
        strncpy(owner_addr, o.c_str(), sizeof(owner_addr) - 1);
        owner_addr[sizeof(owner_addr) - 1] = '\0';
        owner_set = true;
        Serial.printf("BLE: owner loaded = %s\n", owner_addr);
    }
}

// Delete every stored bond that isn't the owner, so the board stays paired to
// exactly one machine. Removing a bond shifts the indices, so restart from 0.
static void prune_foreign_bonds() {
    if (!owner_set) return;
    bool removed;
    do {
        removed = false;
        int n = NimBLEDevice::getNumBonds();
        for (int i = 0; i < n; i++) {
            NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
            if (strcmp(a.toString().c_str(), owner_addr) != 0) {
                Serial.printf("BLE: pruning non-owner bond %s\n", a.toString().c_str());
                NimBLEDevice::deleteBond(a);
                removed = true;
                break;
            }
        }
    } while (removed);
}

static void claim_owner(const std::string& id) {
    strncpy(owner_addr, id.c_str(), sizeof(owner_addr) - 1);
    owner_addr[sizeof(owner_addr) - 1] = '\0';
    owner_set = true;
    save_owner();
    Serial.printf("BLE: owner claimed = %s\n", owner_addr);
    prune_foreign_bonds();
}

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes):
    //   flags (3) + appearance (4) + HID service 0x1812 (4) + name "Clawdmeter" (12)
    //   = 23 bytes. macOS Bluetooth Settings only surfaces BLE-only devices
    //   that explicitly advertise the standard HID service UUID (0x1812) —
    //   without it the device is recognized internally but hidden from the
    //   GUI nearby-devices list.
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    adv->setName(DEVICE_NAME);
    bool ok = adv->start();
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
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

    // Lock the board to a single owner machine. The first machine to bond
    // becomes the owner; any other machine that pairs is un-bonded and dropped
    // so the board stays paired to a single machine. (Wi-Fi fork: this guards
    // the HID keyboard link only — there is no BLE usage-data channel.)
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        std::string id = info.getIdAddress().toString();
        Serial.printf("BLE: auth complete peer=%s bonded=%d enc=%d\n",
            id.c_str(), info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
        if (id == ZERO_ADDR) return;
        if (!owner_set) {
            claim_owner(id);
        } else if (strcmp(id.c_str(), owner_addr) != 0) {
            Serial.printf("BLE: rejecting non-owner %s (owner=%s)\n", id.c_str(), owner_addr);
            NimBLEDevice::deleteBond(info.getIdAddress());
            server->disconnect(info);
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    // Restore the locked owner (if any) and drop any stale non-owner bonds so
    // the board stays paired to a single machine across reboots.
    load_owner();
    prune_foreign_bonds();

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Anthropic");
    // PnP ID: source=BT SIG, vendor=Espressif 0x02E5. Avoids macOS rejecting
    // Apple-vendor-ID spoofers when validating HID device identity.
    hid_dev->setPnp(0x01, 0x02E5, 0x0001, 0x0100);
    // country=33 (US ANSI) prevents macOS Keyboard Setup Assistant on first pair.
    hid_dev->setHidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->getInputReport(1);

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

ble_state_t ble_get_state(void)       { return state; }
const char* ble_get_device_name(void) { return DEVICE_NAME; }
const char* ble_get_mac_address(void) { return mac_str; }

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    clear_owner();  // release ownership so the board can be handed to another machine
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_bonds(void) {
    return NimBLEDevice::getNumBonds() > 0;
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}
