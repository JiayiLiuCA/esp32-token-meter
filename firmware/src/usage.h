#pragma once
// Usage snapshot pushed by the host over BLE (protocol v4, see README).

#include <Arduino.h>

// Same windows as T3 Code's usage page.
enum Period : uint8_t { P_24H = 0, P_7D, P_30D, P_90D, P_COUNT };

struct Usage {
  bool valid = false;
  uint32_t rx_ms = 0;  // millis() when the snapshot arrived
  uint64_t tokens[P_COUNT] = {};
  uint32_t cents[P_COUNT] = {};
  uint32_t sessions[P_COUNT] = {};  // distinct sessions, as T3 Code counts them
};

// Parses one JSON payload into `out`. Returns false (and leaves `out`
// untouched) when the payload is malformed or from an unknown version.
bool usage_parse(const char *json, size_t len, Usage &out);
