#pragma once
// BLE peripheral: one custom service with a single write characteristic the
// host writes a complete JSON payload to (long writes up to 512 bytes).

#include <Arduino.h>

#define TM_DEVICE_NAME "TokenMeter"
#define TM_SERVICE_UUID "7e1a0001-6b2c-4d3e-9f10-746f6b6d7472"
#define TM_RX_UUID "7e1a0002-6b2c-4d3e-9f10-746f6b6d7472"

void ble_begin();
bool ble_connected();
// Copies the latest unread payload into buf (NUL-terminated). Returns its
// length, or 0 when nothing new arrived since the last call.
size_t ble_take_payload(char *buf, size_t cap);
