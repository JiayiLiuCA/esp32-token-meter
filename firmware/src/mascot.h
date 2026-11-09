#pragma once
// Clawd-style pixel mascot for the bottom-right corner: a 22x16-cell 1-bit
// sprite (solid body, cut-out eyes) driven by short procedural actions.
//
// Awake: every 10-15 s a random action (blink, glance, wave, hop, walk,
// think, dance, look up at the number, shake). Sleepy (no new usage for a
// while, set by the caller): eyes stay shut and "z"s drift up now and then.
// mascot_wake() hops; mascot_hop() is the plain "new usage" hop.

#include <U8g2lib.h>

static constexpr int MASCOT_CELL = 2;                 // px per cell
static constexpr int MASCOT_W = 22 * MASCOT_CELL;     // 44
static constexpr int MASCOT_H = 16 * MASCOT_CELL;     // 32

enum Bubble : uint8_t { BUBBLE_NONE = 0, BUBBLE_DOTS, BUBBLE_BANG, BUBBLE_HEART, BUBBLE_Z };

struct MascotPose {
  bool eyes_closed = false;
  int8_t eye_dx = 0;        // cells (glance), -1 / 0 / +1
  int8_t eye_dy = 0;        // cells, -1 = looking up
  int8_t x = 0;             // px offset (walk / shake), negative = left
  int8_t dy = 0;            // px, negative = up
  bool arm_r = false;       // right arm raised
  bool arm_l = false;       // left arm raised
  bool squash = false;      // crouch: body one cell lower, legs one cell shorter
  uint8_t legs = 0;         // 0 = planted, 1 / 2 = alternate leg pair lifted (walk)
  Bubble bubble = BUBBLE_NONE;
  uint8_t bubble_n = 0;     // dots shown (1-3) / z count (1-3)
};

// `sleepy` restores the state kept across deep sleep without the nod-off blink.
void mascot_begin(uint32_t seed, bool sleepy = false);
// Starts one idle action right away (a snore when sleepy).
void mascot_play_now(uint32_t now_ms);
// True while an action is playing; false once it is back in the rest pose.
bool mascot_busy();
// Advances the current action; returns true when the pose changed.
bool mascot_tick(uint32_t now_ms);
// "New usage" hop (also wakes it up).
void mascot_hop(uint32_t now_ms);
// Sleepy mode on/off; turning it off is done by mascot_hop().
void mascot_set_sleepy(bool sleepy, uint32_t now_ms);
const MascotPose &mascot_pose();
// Draws with the resting sprite's bottom-left at (x, bottom). Actions may
// move it up to MASCOT_REACH_LEFT px left and draw a bubble up to
// MASCOT_REACH_UP px above its top.
static constexpr int MASCOT_REACH_LEFT = 17;
static constexpr int MASCOT_REACH_UP = 22;
void mascot_draw(U8G2 &g, int x, int bottom, const MascotPose &p);
