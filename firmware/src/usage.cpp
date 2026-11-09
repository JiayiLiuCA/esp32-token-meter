#include "usage.h"

#include <ArduinoJson.h>

bool usage_parse(const char *json, size_t len, Usage &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if (doc["v"].as<int>() != 4) return false;
  JsonArrayConst periods = doc["p"].as<JsonArrayConst>();
  if (periods.isNull() || periods.size() != P_COUNT) return false;

  Usage u;
  for (int p = 0; p < P_COUNT; p++) {
    JsonArrayConst pair = periods[p].as<JsonArrayConst>();
    if (pair.isNull() || pair.size() != 3) return false;
    u.tokens[p] = pair[0].as<uint64_t>();
    u.cents[p] = pair[1].as<uint32_t>();
    u.sessions[p] = pair[2].as<uint32_t>();
  }
  u.valid = true;
  out = u;
  return true;
}
