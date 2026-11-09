// Token Meter: shows the token and cost totals of T3 Code's usage page
// (Past 24h / 7 / 30 / 90 days) on the Waveshare ESP32-S3-RLCD-4.2.
// The host pushes a snapshot over BLE every 5 minutes.

#include <Arduino.h>
#include <Preferences.h>

#include "ST7305_U8g2.h"
#include "battery.h"
#include "ble_link.h"
#include "mascot.h"
#include "ui.h"
#include "usage.h"

// Pins from Waveshare's factory firmware (examples/factory/main/user_config.h).
static constexpr int PIN_SCK = 11, PIN_MOSI = 12, PIN_DC = 5, PIN_CS = 40, PIN_RST = 41;
static constexpr int PIN_KEY = 18;   // KEY button, active low
static constexpr int PIN_BOOT = 0;   // BOOT button, active low

// Host pushes every 5 min; three missed pushes count as stale.
static constexpr uint32_t STALE_MS = 15 * 60 * 1000;
static constexpr uint32_t BATTERY_MS = 60 * 1000;
// The mascot dozes off when the 24h token count hasn't grown for this long.
static constexpr uint32_t SLEEPY_AFTER_MS = 30 * 60 * 1000;

static ST7305_U8g2 lcd(PIN_SCK, PIN_MOSI, PIN_DC, PIN_CS, PIN_RST);
static Preferences prefs;
static Usage usage;
static UiState ui;
static char rx[520];
static uint32_t last_growth_ms = 0;

static bool button_pressed() {
  static bool was_down = false;
  static uint32_t last_change = 0;
  bool down = digitalRead(PIN_KEY) == LOW || digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();
  if (down != was_down && now - last_change > 40) {
    was_down = down;
    last_change = now;
    return down;
  }
  return false;
}

// Only staleness is drawn; a dropped link alone is not.
static LinkStatus link_status() {
  if (!usage.valid) return LINK_WAITING;
  if (millis() - usage.rx_ms > STALE_MS) return LINK_STALE;
  return LINK_OK;
}

// Bars as clawdmeter picks its battery icon: <=10 % empty, <=35 % one,
// <=75 % two, else three. Coarse steps keep ADC noise from redrawing.
static uint8_t battery_bars() {
  uint8_t p = battery_percent();
  return p <= 10 ? 0 : p <= 35 ? 1 : p <= 75 ? 2 : 3;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  prefs.begin("tokenmeter", false);
  ui.tab = (Period)(prefs.getUChar("tab", P_24H) % P_COUNT);

  battery_begin();
  ui.battery_bars = battery_bars();
  Serial.printf("tokenmeter: battery %u mV, %u bars\n", battery_millivolts(), ui.battery_bars);

  mascot_begin(esp_random());

  lcd.begin(0, U8G2_R1);  // landscape 400x300
  ble_begin();
  uint32_t t0 = micros();
  ui_draw(*lcd.getU8g2(), usage, ui);
  Serial.printf("tokenmeter: ready (full redraw %lu ms)\n", (unsigned long)((micros() - t0) / 1000));
}

void loop() {
  bool dirty = false;

  size_t n = ble_take_payload(rx, sizeof rx);
  if (n > 0) {
    uint64_t before = usage.valid ? usage.tokens[P_24H] : 0;
    bool had = usage.valid;
    if (usage_parse(rx, n, usage)) {
      dirty = true;
      if (!had) last_growth_ms = millis();
      if (had && usage.tokens[P_24H] > before) {  // new usage: celebrate (and wake up)
        last_growth_ms = millis();
        mascot_hop(millis());
      }
      Serial.printf("tokenmeter: snapshot ok, 24h %llu tok %lu c\n",
                    (unsigned long long)usage.tokens[P_24H], (unsigned long)usage.cents[P_24H]);
    } else {
      Serial.printf("tokenmeter: bad payload (%u bytes)\n", (unsigned)n);
    }
  }

  LinkStatus st = link_status();
  if (st != ui.status) {
    ui.status = st;
    dirty = true;
  }

  static uint32_t last_battery = 0;
  if (millis() - last_battery > BATTERY_MS) {
    last_battery = millis();
    uint8_t b = battery_bars();
    if (b != ui.battery_bars) {
      ui.battery_bars = b;
      dirty = true;
    }
  }

  if (button_pressed()) {
    ui.tab = (Period)((ui.tab + 1) % P_COUNT);
    prefs.putUChar("tab", ui.tab);
    dirty = true;
  }

  if (usage.valid) mascot_set_sleepy(millis() - last_growth_ms > SLEEPY_AFTER_MS, millis());
  if (mascot_tick(millis())) dirty = true;

  if (dirty) {
    ui.mascot = mascot_pose();
    ui_draw(*lcd.getU8g2(), usage, ui);
  }
  delay(20);
}
