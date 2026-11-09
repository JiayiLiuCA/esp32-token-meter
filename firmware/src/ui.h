#pragma once

#include <U8g2lib.h>

#include "mascot.h"
#include "usage.h"

// Shown only when something is wrong; normal operation draws no status.
enum LinkStatus : uint8_t { LINK_OK = 0, LINK_WAITING, LINK_NONE, LINK_STALE };

struct UiState {
  Period tab = P_24H;
  LinkStatus status = LINK_WAITING;
  uint8_t battery_bars = 0;  // 0..3, clawdmeter thresholds (see main.cpp)
  MascotPose mascot;
};

void ui_draw(U8G2 &g, const Usage &u, const UiState &s);
