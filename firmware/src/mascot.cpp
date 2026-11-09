#include "mascot.h"

#include <Arduino.h>

// Resting silhouette, 22x16 cells ('#' = body). Symmetric take on Clawd:
// head/body block, one arm each side, four legs. Eyes are cut out at draw
// time so they can blink and glance.
static const char *const BODY[16] = {
    "...################...",
    "...################...",
    "...################...",
    "...################...",
    "######################",
    "######################",
    "######################",
    "######################",
    "...################...",
    "...################...",
    "...################...",
    "...################...",
    "...##..##....##..##...",
    "...##..##....##..##...",
    "...##..##....##..##...",
    "...##..##....##..##...",
};
static constexpr int EYE_ROW = 2, EYE_H = 2, EYE_L = 5, EYE_R = 15, EYE_W = 2;
static constexpr uint32_t SLEEPY_Z_MIN_MS = 8000, SLEEPY_Z_SPREAD_MS = 10000;

// ---- action player: a list of (pose, hold ms) steps ----

struct Step { MascotPose pose; uint16_t ms; };

static constexpr int MAX_STEPS = 48;
static Step s_steps[MAX_STEPS];
static int s_n = 0, s_i = 0;
static uint32_t s_step_start = 0, s_next_idle = 0;
static MascotPose s_pose;
static bool s_sleepy = false;

static MascotPose rest() {
  MascotPose p;
  p.eyes_closed = s_sleepy;
  return p;
}

// Builder so actions read as a sequence of poses.
struct Seq {
  int n = 0;
  Step st[MAX_STEPS];
  MascotPose &add(uint16_t ms) {
    st[n].pose = rest();
    st[n].ms = ms;
    return st[n++].pose;
  }
};

static void play(const Seq &q, uint32_t now) {
  memcpy(s_steps, q.st, sizeof(Step) * q.n);
  s_n = q.n; s_i = 0; s_step_start = now;
  s_pose = s_steps[0].pose;
}

static void schedule_idle(uint32_t now) {
  s_next_idle = now + (s_sleepy ? SLEEPY_Z_MIN_MS + random(SLEEPY_Z_SPREAD_MS) : 10000 + random(5000));
}

// ---- actions ----

static void act_blink(uint32_t now, int times) {
  Seq q;
  for (int i = 0; i < times; i++) {
    q.add(120).eyes_closed = true;
    if (i + 1 < times) q.add(160);
  }
  q.add(1);
  play(q, now);
}

static void act_glance(uint32_t now) {
  int d = random(2) ? 1 : -1;
  Seq q;
  q.add(1400).eye_dx = d;
  { MascotPose &p = q.add(120); p.eye_dx = d; p.eyes_closed = true; }
  q.add(1100).eye_dx = -d;
  q.add(1);
  play(q, now);
}

static void act_wave(uint32_t now) {
  Seq q;
  for (int i = 0; i < 3; i++) { q.add(220).arm_r = true; q.add(180); }
  q.add(1);
  play(q, now);
}

static void add_hop(Seq &q, bool eyes_shut_at_top) {
  { MascotPose &p = q.add(120); p.dy = MASCOT_CELL; p.squash = true; }
  q.add(70).dy = -4;
  { MascotPose &p = q.add(110); p.dy = -6; p.eyes_closed = eyes_shut_at_top; }
  q.add(70).dy = -4;
  { MascotPose &p = q.add(120); p.dy = MASCOT_CELL; p.squash = true; }
}

static void act_hop(uint32_t now) {
  Seq q;
  add_hop(q, true);
  q.add(1);
  play(q, now);
}

// Walk left a few steps, look back at the numbers, walk home.
static void act_walk(uint32_t now) {
  Seq q;
  const int steps = 8, stride = 2;  // 16 px: clears a 5-digit sessions value
  for (int i = 1; i <= steps; i++) {
    MascotPose &p = q.add(140);
    p.x = -i * stride; p.legs = 1 + (i & 1); p.eye_dx = -1;
    p.dy = (i & 1) ? -1 : 0;
  }
  { MascotPose &p = q.add(900); p.x = -steps * stride; p.eye_dx = -1; p.eye_dy = -1; }
  { MascotPose &p = q.add(120); p.x = -steps * stride; p.eyes_closed = true; }
  { MascotPose &p = q.add(500); p.x = -steps * stride; p.eye_dx = 1; }
  for (int i = steps - 1; i >= 0; i--) {
    MascotPose &p = q.add(140);
    p.x = -i * stride; p.legs = 1 + (i & 1); p.eye_dx = 1;
    p.dy = (i & 1) ? -1 : 0;
  }
  q.add(1);
  play(q, now);
}

// Thought bubble: dots appear one by one, twice, then "!" and a hop.
static void act_think(uint32_t now) {
  Seq q;
  for (int round = 0; round < 2; round++) {
    for (int d = 1; d <= 3; d++) {
      MascotPose &p = q.add(380);
      p.bubble = BUBBLE_DOTS; p.bubble_n = d; p.eye_dy = -1; p.eye_dx = round ? -1 : 1;
    }
  }
  q.add(500).bubble = BUBBLE_BANG;
  add_hop(q, false);
  for (int i = 0; i < 5; i++) q.st[q.n - 1 - i].pose.bubble = BUBBLE_BANG;
  q.add(1);
  play(q, now);
}

// Arms up in turn with a little bob.
static void act_dance(uint32_t now) {
  Seq q;
  for (int i = 0; i < 8; i++) {
    MascotPose &p = q.add(200);
    p.arm_l = i & 1; p.arm_r = !(i & 1); p.dy = (i & 1) ? -1 : 0;
    p.eyes_closed = i == 3 || i == 4;
    p.x = (i & 1) ? -1 : 1;
  }
  q.add(1);
  play(q, now);
}

// Look up-left at the big number, then a heart.
static void act_admire(uint32_t now) {
  Seq q;
  { MascotPose &p = q.add(1300); p.eye_dx = -1; p.eye_dy = -1; }
  { MascotPose &p = q.add(1400); p.eye_dx = -1; p.eye_dy = -1; p.bubble = BUBBLE_HEART; }
  { MascotPose &p = q.add(120); p.eyes_closed = true; p.bubble = BUBBLE_HEART; }
  q.add(1);
  play(q, now);
}

static void act_shake(uint32_t now) {
  Seq q;
  for (int i = 0; i < 8; i++) q.add(60).x = (i & 1) ? -1 : 1;
  q.add(1);
  play(q, now);
}

// Sleepy: z's drift up one by one, eyes stay shut.
static void act_snore(uint32_t now) {
  Seq q;
  for (int z = 1; z <= 3; z++) { MascotPose &p = q.add(700); p.bubble = BUBBLE_Z; p.bubble_n = z; }
  { MascotPose &p = q.add(900); p.bubble = BUBBLE_Z; p.bubble_n = 3; }
  q.add(1);
  play(q, now);
}

static void start_idle(uint32_t now) {
  if (s_sleepy) { act_snore(now); return; }
  switch (random(16)) {
    case 0: case 1: act_blink(now, 1); break;
    case 2: act_blink(now, 2); break;
    case 3: case 4: act_glance(now); break;
    case 5: act_wave(now); break;
    case 6: act_hop(now); break;
    case 7: case 8: act_walk(now); break;
    case 9: case 10: act_think(now); break;
    case 11: case 12: act_dance(now); break;
    case 13: case 14: act_admire(now); break;
    default: act_shake(now); break;
  }
}

// ---- public ----

void mascot_begin(uint32_t seed) {
  randomSeed(seed);
  s_sleepy = false;
  s_pose = rest();
  s_n = 0;
  schedule_idle(millis());
}

void mascot_hop(uint32_t now) {
  s_sleepy = false;
  act_hop(now);
  schedule_idle(now);
}

void mascot_set_sleepy(bool sleepy, uint32_t now) {
  if (sleepy == s_sleepy) return;
  s_sleepy = sleepy;
  if (s_n == 0) s_pose = rest();
  // Nod off with a slow double blink before the eyes stay shut.
  if (sleepy) act_blink(now, 2);
  schedule_idle(now);
}

bool mascot_tick(uint32_t now) {
  if (s_n == 0) {
    if ((int32_t)(now - s_next_idle) < 0) return false;
    start_idle(now);
    schedule_idle(now);
    return true;
  }
  if (now - s_step_start < s_steps[s_i].ms) return false;
  s_step_start = now;
  if (++s_i >= s_n) {
    s_n = 0;
    s_pose = rest();
    schedule_idle(now);  // the still gap is measured from the end of an action
  } else {
    s_pose = s_steps[s_i].pose;
  }
  return true;
}

const MascotPose &mascot_pose() { return s_pose; }

// ---- drawing ----

static const uint8_t HEART[7] = {0x36, 0x7f, 0x7f, 0x7f, 0x3e, 0x1c, 0x08};  // 7x7, bit 6 = left

static void draw_bubble(U8G2 &g, int head_x, int head_top, const MascotPose &p) {
  // Speech bubble above the head, a little right, with a stepped tail.
  const int bw = 22, bh = 13;
  const int bx = head_x + 22, by = head_top - bh - 7;
  if (p.bubble == BUBBLE_DOTS || p.bubble == BUBBLE_BANG || p.bubble == BUBBLE_HEART) {
    g.drawRFrame(bx, by, bw, bh, 3);
    g.drawPixel(bx + 4, by + bh);
    g.drawPixel(bx + 3, by + bh + 1);
    g.drawPixel(bx + 1, by + bh + 3);
  }
  if (p.bubble == BUBBLE_DOTS) {
    for (int d = 0; d < p.bubble_n; d++) g.drawBox(bx + 5 + d * 5, by + 6, 2, 2);
  } else if (p.bubble == BUBBLE_BANG) {
    g.drawBox(bx + bw / 2 - 1, by + 3, 2, 5);
    g.drawBox(bx + bw / 2 - 1, by + 9, 2, 2);
  } else if (p.bubble == BUBBLE_HEART) {
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 7; c++)
        if (HEART[r] & (0x40 >> c)) g.drawPixel(bx + 7 + c, by + 3 + r);
  } else if (p.bubble == BUBBLE_Z) {
    // Rising z's, each a bit larger and further up-right.
    g.setFont(u8g2_font_5x7_tr);
    if (p.bubble_n >= 1) g.drawStr(head_x + 30, head_top - 2, "z");
    if (p.bubble_n >= 2) g.drawStr(head_x + 36, head_top - 9, "z");
    g.setFont(u8g2_font_helvB08_tr);
    if (p.bubble_n >= 3) g.drawStr(head_x + 42, head_top - 15, "Z");
  }
}

void mascot_draw(U8G2 &g, int x0, int bottom, const MascotPose &p) {
  const int C = MASCOT_CELL;
  const int x = x0 + p.x;
  const int top = bottom - MASCOT_H + p.dy;
  for (int r = 0; r < 16; r++) {
    for (int c = 0; c < 22; c++) {
      if (BODY[r][c] != '#') continue;
      // Crouch: body drops one cell (pose dy = +C) and the legs lose their
      // last row, so the feet stay on the ground.
      if (p.squash && r == 15) continue;
      // Walk: one diagonal pair of legs is lifted (drawn one row short).
      if (r == 15 && p.legs) {
        bool pair_a = c <= 4 || (c >= 13 && c <= 14);
        if ((p.legs == 1) == pair_a) continue;
      }
      // Raised arms keep their shoulder (row 4) and point up.
      if (p.arm_r && r >= 5 && r <= 7 && c >= 19) continue;
      if (p.arm_l && r >= 5 && r <= 7 && c <= 2) continue;
      g.drawBox(x + c * C, top + r * C, C, C);
    }
  }
  // Forearms raised beside the head, one cell clear of it.
  if (p.arm_r) for (int r = 1; r < 4; r++) g.drawBox(x + 20 * C, top + r * C, 2 * C, C);
  if (p.arm_l) for (int r = 1; r < 4; r++) g.drawBox(x, top + r * C, 2 * C, C);

  // Eyes: cut out of the body.
  g.setDrawColor(0);
  for (int e = 0; e < 2; e++) {
    int ex = (e ? EYE_R : EYE_L) + p.eye_dx;
    int ey = EYE_ROW + p.eye_dy;
    if (p.eyes_closed) {
      g.drawBox(x + ex * C, top + (ey + EYE_H - 1) * C + 1, EYE_W * C, 1);
    } else {
      g.drawBox(x + ex * C, top + ey * C, EYE_W * C, EYE_H * C);
    }
  }
  g.setDrawColor(1);

  if (p.bubble != BUBBLE_NONE) draw_bubble(g, x, top, p);
}
