#include "battery.h"

static constexpr int PIN_BAT = 4;
static constexpr int DIVIDER = 3;
static constexpr int EMPTY_MV = 3000, FULL_MV = 4120;

void battery_begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);
}

uint16_t battery_millivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT);
  return (uint16_t)(sum / 16 * DIVIDER);
}

uint8_t battery_percent() {
  int mv = battery_millivolts();
  if (mv <= EMPTY_MV) return 0;
  if (mv >= FULL_MV) return 100;
  return (uint8_t)((mv - EMPTY_MV) * 100 / (FULL_MV - EMPTY_MV));
}
