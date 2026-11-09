#pragma once
// BLE peripheral: one custom service with a single write characteristic the
// host writes a complete JSON payload to (long writes up to 512 bytes).
//
// The board drives the cadence: it advertises only inside a "sync window"
// (ble_open_window .. ble_close_window). The host scans continuously,
// connects when it sees the board, writes one snapshot, and the board then
// closes the window (disconnects everyone, stops advertising).

#include <Arduino.h>

#define TM_DEVICE_NAME "TokenMeter"
#define TM_SERVICE_UUID "7e1a0001-6b2c-4d3e-9f10-746f6b6d7472"
#define TM_RX_UUID "7e1a0002-6b2c-4d3e-9f10-746f6b6d7472"

void ble_begin();  // stack + GATT only, not advertising
void ble_open_window();
void ble_close_window();
bool ble_window_open();
// Copies the latest unread payload into buf (NUL-terminated). Returns its
// length, or 0 when nothing new arrived since the last call.
size_t ble_take_payload(char *buf, size_t cap);
