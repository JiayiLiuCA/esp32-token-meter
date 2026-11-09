#include "ble_link.h"

#include <NimBLEDevice.h>

static constexpr size_t RX_MAX = 512;

// Power: the host pushes one snapshot every 5 minutes, so advertising need
// not be fast. NimBLE's default is 30-60 ms; this is ~1.0-1.2 s (0.625 ms
// units). The host scans for 10 s, so it still finds the board.
//
// The connection interval is deliberately left to the central. Asking for a
// slower one (updateConnParams, e.g. 300-500 ms) made the Linux host drop the
// link exactly 40 s after every connect: the kernel rejects anything above
// le_conn_max_interval (default 70 ms) and the LL procedure then times out.
static constexpr uint16_t ADV_ITVL_MIN = 1600, ADV_ITVL_MAX = 1920;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_buf[RX_MAX + 1];
static size_t s_len = 0;
static bool s_fresh = false;
static volatile int s_conns = 0;

class ServerCb : public NimBLEServerCallbacks {
  // Keep advertising while a slot is free: the OS often holds a link of its
  // own (e.g. connected from the Bluetooth settings), and the host app must
  // still be able to find and connect to the board.
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    s_conns = s_conns + 1;
    if (s_conns < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    s_conns = s_conns > 0 ? s_conns - 1 : 0;
    NimBLEDevice::startAdvertising();
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
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  server->advertiseOnDisconnect(true);

  NimBLEService *svc = server->createService(TM_SERVICE_UUID);
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
  bool ok = adv->start();
  Serial.printf("tokenmeter: advertising %s\n", ok ? "started" : "FAILED");
}

bool ble_connected() { return s_conns > 0; }

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
