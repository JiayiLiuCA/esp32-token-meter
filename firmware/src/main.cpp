// Token Meter: shows the token and cost totals of T3 Code's usage page
// (Past 24h / 7 / 30 / 90 days) on the Waveshare ESP32-S3-RLCD-4.2.
//
// Power: on battery the board deep-sleeps between syncs. Every sync period
// (SYNC_FAST_MS while usage is growing, SYNC_SLOW_MS once it has been flat
// for ACTIVE_FOR_MS) it wakes, opens a BLE sync window (the host is scanning and pushes one
// snapshot), redraws, plays one mascot action and sleeps again; the panel
// keeps its image through deep sleep. KEY/BOOT wake it to switch the period.
// While a computer is on the USB port (SOF frames seen) it stays awake and
// animates continuously instead and syncs every SYNC_FAST_MS (no battery
// to save); this also keeps the serial port up for flashing.

#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include "ST7305_U8g2.h"
#include "battery.h"
#include "ble_link.h"
#include "mascot.h"
#include "ui.h"
#include "usage.h"

// Pins from Waveshare's factory firmware (examples/factory/main/user_config.h).
static constexpr int PIN_SCK = 11, PIN_MOSI = 12, PIN_DC = 5, PIN_CS = 40, PIN_RST = 41;
static constexpr int PIN_KEY = 18;   // KEY button, active low (RTC GPIO, ext1 wake)
static constexpr int PIN_BOOT = 0;   // BOOT button, active low (RTC GPIO, ext1 wake)

// Sync every minute while tokens are being spent (or on USB power), every
// 5 minutes once no period's count has grown for ACTIVE_FOR_MS.
static constexpr uint32_t SYNC_FAST_MS = 60 * 1000;
static constexpr uint32_t SYNC_SLOW_MS = 5 * 60 * 1000;
static constexpr int64_t ACTIVE_FOR_MS = 10 * 60 * 1000;
// Give up on a sync after this long (host not running / out of range).
static constexpr uint32_t SYNC_WINDOW_MS = 20 * 1000;
// Let the write response reach the host before the board disconnects.
static constexpr uint32_t SYNC_LINGER_MS = 150;
// After a button wake, stay up this long after the last press.
static constexpr uint32_t BUTTON_LINGER_MS = 5 * 1000;
// usb_serial_jtag_is_connected() needs a moment after boot to see SOFs.
static constexpr uint32_t USB_DETECT_MS = 1500;
// Three missed slow syncs count as stale.
static constexpr int64_t STALE_MS = 15 * 60 * 1000;
static constexpr uint32_t BATTERY_MS = 60 * 1000;
// The mascot dozes off when no period's token count has grown for this long.
static constexpr int64_t SLEEPY_AFTER_MS = 30 * 60 * 1000;

// Survives deep sleep (not power loss / reset); `magic` tells a valid state
// apart. Every member needs an initializer and the variable must be
// constinit: otherwise the compiler emits a static constructor that runs on
// every wake and wipes the RTC copy (it did: data vanished after each wake).
struct Persist {
  uint32_t magic = 0;
  Usage usage;
  int64_t last_growth_ms = 0;  // rtc_ms()
  int64_t sync_ms = 0;         // rtc_ms() the last sync window opened
};
static constexpr uint32_t PERSIST_MAGIC = 0x746d7032;  // "tmp2"
RTC_DATA_ATTR static constinit Persist rtc;

static ST7305_U8g2 lcd(PIN_SCK, PIN_MOSI, PIN_DC, PIN_CS, PIN_RST);
static Preferences prefs;
static UiState ui;
static char rx[520];
static Usage &usage = rtc.usage;

static uint32_t sync_started = 0;   // millis() the current window opened
static uint32_t synced_at = 0;      // millis() the payload arrived, 0 = none yet
static uint32_t awake_until = 0;    // millis(); button linger

// Wall-ish clock that keeps running through deep sleep (RTC timer).
static int64_t rtc_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static bool button_down() { return digitalRead(PIN_KEY) == LOW || digitalRead(PIN_BOOT) == LOW; }

static bool was_down = false;
static bool button_pressed() {
  static uint32_t last_change = 0;
  bool down = button_down();
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
  if (rtc_ms() - usage.rx_ms > STALE_MS) return LINK_STALE;
  return LINK_OK;
}

// Bars as clawdmeter picks its battery icon: <=10 % empty, <=35 % one,
// <=75 % two, else three. Coarse steps keep ADC noise from redrawing.
static uint8_t battery_bars() {
  uint8_t p = battery_percent();
  return p <= 10 ? 0 : p <= 35 ? 1 : p <= 75 ? 2 : 3;
}

static bool sleepy_now() { return usage.valid && rtc_ms() - rtc.last_growth_ms > SLEEPY_AFTER_MS; }

// Evaluated when deciding to sync or sleep, so the cadence follows activity
// and USB as they change; the next sync is always relative to the last one.
static int64_t next_sync_ms() {
  bool active = usage.valid && rtc_ms() - rtc.last_growth_ms < ACTIVE_FOR_MS;
  bool fast = active || HWCDC::isPlugged();
  return rtc.sync_ms + (fast ? SYNC_FAST_MS : SYNC_SLOW_MS);
}

static void next_tab() {
  ui.tab = (Period)((ui.tab + 1) % P_COUNT);
  prefs.putUChar("tab", ui.tab);
}

static void open_sync() {
  sync_started = millis();
  synced_at = 0;
  rtc.sync_ms = rtc_ms();
  ble_open_window();
}

static void go_to_sleep() {
  ble_close_window();
  prefs.end();
  lcd.setLowPower(true);
  // Keep the panel selected-off and out of reset while the chip sleeps.
  gpio_hold_en((gpio_num_t)PIN_RST);
  gpio_hold_en((gpio_num_t)PIN_CS);
  gpio_deep_sleep_hold_en();

  int64_t left = next_sync_ms() - rtc_ms();
  if (left < 1000) left = 1000;
  esp_sleep_enable_timer_wakeup((uint64_t)left * 1000);

  // Buttons wake via ext1 (any low); keep their pull-ups on in sleep.
  const uint64_t mask = (1ULL << PIN_KEY) | (1ULL << PIN_BOOT);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  for (int pin : {PIN_KEY, PIN_BOOT}) {
    rtc_gpio_pullup_en((gpio_num_t)pin);
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
  }
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.printf("tokenmeter: sleeping %lld s\n", (long long)(left / 1000));
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  // Power: 80 MHz is the lowest clock BLE allows and roughly halves the
  // core's draw versus the default 240 MHz. APB stays 80 MHz, so SPI and
  // the ADC are unaffected; a full redraw goes from ~12 ms to ~22 ms.
  setCpuFrequencyMhz(80);
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool timer_wake = cause == ESP_SLEEP_WAKEUP_TIMER;
  bool button_wake = cause == ESP_SLEEP_WAKEUP_EXT1;
  bool cold = !(timer_wake || button_wake) || rtc.magic != PERSIST_MAGIC;
  if (cold) {
    memset(&rtc, 0, sizeof rtc);
    rtc.usage = Usage();
    rtc.magic = PERSIST_MAGIC;
    timer_wake = button_wake = false;
  }

  // Buttons back from RTC to digital; panel pins driven to their held
  // levels before the hold is released.
  rtc_gpio_deinit((gpio_num_t)PIN_KEY);
  rtc_gpio_deinit((gpio_num_t)PIN_BOOT);
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  gpio_hold_dis((gpio_num_t)PIN_RST);
  gpio_hold_dis((gpio_num_t)PIN_CS);
  gpio_deep_sleep_hold_dis();
  was_down = button_down();  // the press that woke us is handled below, once

  prefs.begin("tokenmeter", false);
  ui.tab = (Period)(prefs.getUChar("tab", P_24H) % P_COUNT);
  if (button_wake) {
    next_tab();
    awake_until = millis() + BUTTON_LINGER_MS;
  }

  battery_begin();
  ui.battery_bars = battery_bars();
  ui.status = link_status();

  mascot_begin(esp_random(), sleepy_now());

  lcd.begin(0, U8G2_R1, cold);  // landscape 400x300
  if (!cold) lcd.setLowPower(false);
  ble_begin();

  // A button wake doesn't move the sync cadence, unless a sync is due anyway.
  if (cold || timer_wake || rtc_ms() >= next_sync_ms()) open_sync();
  if (!button_wake) mascot_play_now(millis());

  ui.mascot = mascot_pose();
  uint32_t t0 = micros();
  ui_draw(*lcd.getU8g2(), usage, ui);
  Serial.printf("tokenmeter: %s, battery %u mV, redraw %lu ms\n",
                cold ? "cold boot" : timer_wake ? "timer wake" : "button wake",
                battery_millivolts(), (unsigned long)((micros() - t0) / 1000));
}

void loop() {
  bool dirty = false;
  uint32_t now = millis();

  size_t n = ble_take_payload(rx, sizeof rx);
  if (n > 0) {
    const Usage before = usage;
    if (usage_parse(rx, n, usage)) {
      usage.rx_ms = rtc_ms();
      synced_at = now ? now : 1;
      dirty = true;
      // Any period growing means new usage. 24h alone can shrink while
      // tokens are spent (heavier usage rolling off the far end), and the
      // calendar periods drop at midnight, so check them all.
      bool grew = false;
      for (int p = 0; p < P_COUNT; p++) grew |= usage.tokens[p] > before.tokens[p];
      if (!before.valid) rtc.last_growth_ms = rtc_ms();
      if (before.valid && grew) {  // new usage: celebrate (and wake up)
        rtc.last_growth_ms = rtc_ms();
        mascot_hop(now);
      }
      Serial.printf("tokenmeter: snapshot ok, 24h %llu tok %lu c\n",
                    (unsigned long long)usage.tokens[P_24H], (unsigned long)usage.cents[P_24H]);
    } else {
      Serial.printf("tokenmeter: bad payload (%u bytes)\n", (unsigned)n);
    }
  }

  if (ble_window_open()) {
    bool done = synced_at && now - synced_at >= SYNC_LINGER_MS;
    bool timed_out = now - sync_started >= SYNC_WINDOW_MS;
    if (done || timed_out) {
      if (timed_out && !synced_at) Serial.println("tokenmeter: no snapshot this window");
      ble_close_window();
    }
  } else if (rtc_ms() >= next_sync_ms()) {
    open_sync();  // only reached while staying awake on USB
  }

  LinkStatus st = link_status();
  if (st != ui.status) {
    ui.status = st;
    dirty = true;
  }

  static uint32_t last_battery = 0;
  if (now - last_battery > BATTERY_MS) {
    last_battery = now;
    uint8_t b = battery_bars();
    if (b != ui.battery_bars) {
      ui.battery_bars = b;
      dirty = true;
    }
  }

  if (button_pressed()) {
    next_tab();
    awake_until = now + BUTTON_LINGER_MS;
    dirty = true;
  }

  mascot_set_sleepy(sleepy_now(), now);
  if (mascot_tick(now)) dirty = true;

  if (dirty) {
    ui.mascot = mascot_pose();
    ui_draw(*lcd.getU8g2(), usage, ui);
  }

  // Sleep once the window is closed, the action has finished (rest pose
  // drawn above), and no button is held or recently pressed. Stay awake
  // while a computer is on the USB port.
  bool usb_host = HWCDC::isPlugged();
  if (now > USB_DETECT_MS && !usb_host && !ble_window_open() && !mascot_busy() &&
      (int32_t)(now - awake_until) >= 0 && !button_down()) {
    go_to_sleep();
  }
  delay(20);
}
