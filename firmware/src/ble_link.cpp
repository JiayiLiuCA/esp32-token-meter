#include "ble_link.h"

#include <NimBLEDevice.h>

static constexpr size_t RX_MAX = 512;

// Advertising only runs inside a sync window that lasts a few seconds, so it
// can be fast: 100-150 ms (0.625 ms units) lets a continuously scanning host
// find the board within a fraction of a second.
//
// The connection interval is deliberately left to the central. Asking for a
// slower one (updateConnParams, e.g. 300-500 ms) made the Linux host drop the
// link exactly 40 s after every connect: the kernel rejects anything above
// le_conn_max_interval (default 70 ms) and the LL procedure then times out.
static constexpr uint16_t ADV_ITVL_MIN = 160, ADV_ITVL_MAX = 240;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_buf[RX_MAX + 1];
static size_t s_len = 0;
static bool s_fresh = false;
static volatile int s_conns = 0;
static volatile bool s_window = false;
static NimBLEServer *s_server = nullptr;

class ServerCb : public NimBLEServerCallbacks {
  // Inside a window, keep advertising while a slot is free: the OS often
  // holds a link of its own (e.g. connected from the Bluetooth settings), and
  // the host app must still be able to find and connect to the board.
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    s_conns = s_conns + 1;
    if (s_window && s_conns < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    s_conns = s_conns > 0 ? s_conns - 1 : 0;
    if (s_window) NimBLEDevice::startAdvertising();
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  // Runs on the NimBLE host task; the main loop picks the payload up.
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.length();
    if (n == 0 || n > RX_MAX) return;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_buf, v.data(), n);
    s_buf[n] = 0;
    s_len = n;
    s_fresh = true;
    portEXIT_CRITICAL(&s_mux);
  }
};

void ble_begin() {
  NimBLEDevice::init(TM_DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCb());
  s_server->advertiseOnDisconnect(false);  // ServerCb decides, per window

  NimBLEService *svc = s_server->createService(TM_SERVICE_UUID);
  NimBLECharacteristic *rx = svc->createCharacteristic(
      TM_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, RX_MAX);
  rx->setCallbacks(new RxCb());
  svc->start();

  // Flags + a 128-bit UUID take 21 of the 31 advertising bytes, so the name
  // goes in the scan response.
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData adv_data;
  adv_data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  adv_data.addServiceUUID(NimBLEUUID(TM_SERVICE_UUID));
  adv->setAdvertisementData(adv_data);
  NimBLEAdvertisementData scan_data;
  scan_data.setName(TM_DEVICE_NAME);
  adv->setScanResponseData(scan_data);
  adv->setMinInterval(ADV_ITVL_MIN);
  adv->setMaxInterval(ADV_ITVL_MAX);
}

void ble_open_window() {
  s_window = true;
  bool ok = NimBLEDevice::startAdvertising();
  Serial.printf("tokenmeter: sync window open (advertising %s)\n", ok ? "ok" : "FAILED");
}

void ble_close_window() {
  if (!s_window) return;
  s_window = false;
  NimBLEDevice::stopAdvertising();
  for (uint16_t h : s_server->getPeerDevices()) s_server->disconnect(h);
  Serial.println("tokenmeter: sync window closed");
}

bool ble_window_open() { return s_window; }

size_t ble_take_payload(char *buf, size_t cap) {
  size_t n = 0;
  portENTER_CRITICAL(&s_mux);
  if (s_fresh && s_len < cap) {
    memcpy(buf, s_buf, s_len + 1);
    n = s_len;
  }
  s_fresh = false;
  portEXIT_CRITICAL(&s_mux);
  return n;
}
