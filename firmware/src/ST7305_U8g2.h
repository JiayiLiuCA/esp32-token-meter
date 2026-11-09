// Copied from Waveshare ESP32-S3-RLCD-4.2 examples (arduino/examples/10_U8G2_Test),
// https://github.com/waveshareteam/ESP32-S3-RLCD-4.2 — Apache License 2.0.
// Modified: begin() can skip the panel reset/init (the panel keeps its image
// and settings through ESP32 deep sleep), and setLowPower() switches the
// panel between high-power (0x38) and low-power (0x39) refresh.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

class ST7305_U8g2 {
private:
  int _sck;
  int _mosi;
  int _dc;
  int _cs;
  int _rst;
  SPIClass *_spi = nullptr;
  U8G2 u8g2_wrapper;
  uint8_t *_my_buf = nullptr;

  void _cmd(uint8_t cmd);
  void _data(const uint8_t *data, size_t len);
  void _cmd_data(uint8_t cmd, const uint8_t *data, size_t len);

  static uint8_t u8x8_d_st7305_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
  static uint8_t u8x8_byte_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

public:
  ST7305_U8g2(int sck = 11, int mosi = 12, int dc = 5, int cs = 40, int rst = 41);
  ~ST7305_U8g2();

  // tile_buf_height 0 means full-buffer mode, about 15 KB RAM for 300x400.
  // A smaller value can be used with firstPage()/nextPage(), but this demo uses full-buffer mode.
  // init_panel = false: only set up SPI and the frame buffer, leaving the
  // panel as it is (used when waking from deep sleep).
  void begin(uint8_t tile_buf_height = 0, const u8g2_cb_t *rotation = U8G2_R0, bool init_panel = true);
  void reset();
  void fullInit();
  void setLowPower(bool on) { _cmd(on ? 0x39 : 0x38); }

  U8G2 *getU8g2()
  {
    return &u8g2_wrapper;
  }
};
