// 400x300 1-bit layout, draw colour 1 = ink. Left-aligned, no rules,
// 24 px margins on every side.
//
//   16.5B                        [===]   <- tokens (generated Logisoso 130/110);
//                                           battery top aligned with the number
//   tokens  /  30 days                   <- caption: period (+ stale flag)
//
//   API cost          sessions           <- labels in the caption's style
//   $11,660.76        837                <- values on the bottom margin

#include "ui.h"

#include "bigfont.h"

static constexpr int W = 400, H = 300;
static constexpr int M = 24;                  // margin on every side
static constexpr int NUM_TOP = M;
static constexpr int NUM_BASE = NUM_TOP + 130;
static constexpr int CAPTION_Y = NUM_BASE + 26;
static constexpr int FOOT_Y = H - M;          // value baseline
static constexpr int FOOT_LABEL_Y = FOOT_Y - 36;
static constexpr int COL2 = 214;

static const char *const PERIOD_LABELS[P_COUNT] = {"Past 24h", "7 days", "30 days", "90 days"};

// Battery: 22x11 1 px outline, one continuous fill stepping in thirds
// (battery_bars 0..3), no percent. Top edge on the number's top line.
static constexpr int BAT_W = 22, BAT_H = 11, BAT_X = W - M - 2 - BAT_W, BAT_Y = NUM_TOP;

static void draw_battery(U8G2 &g, const UiState &s) {
  g.drawRFrame(BAT_X, BAT_Y, BAT_W, BAT_H, 2);
  g.drawBox(BAT_X + BAT_W, BAT_Y + 3, 2, BAT_H - 6);
  int fill = (BAT_W - 4) * s.battery_bars / 3;
  if (fill > 0) g.drawBox(BAT_X + 2, BAT_Y + 2, fill, BAT_H - 4);
}

// ---- formatting ----

// Tokens split into a numeric part and a unit; the unit is drawn smaller.
static void fmt_tokens(char *num, size_t n, const char **unit, uint64_t t) {
  if (t < 1000ULL) { snprintf(num, n, "%llu", (unsigned long long)t); *unit = ""; }
  else if (t < 1000000ULL) { snprintf(num, n, "%.1f", t / 1e3); *unit = "K"; }
  else if (t < 1000000000ULL) { snprintf(num, n, "%.1f", t / 1e6); *unit = "M"; }
  else { snprintf(num, n, "%.1f", t / 1e9); *unit = "B"; }
}

// 1234567 -> "1,234,567"
static void fmt_grouped(char *out, size_t n, unsigned long v) {
  char digits[16];
  snprintf(digits, sizeof digits, "%lu", v);
  int len = strlen(digits), j = 0;
  for (int i = 0; i < len && j < (int)n - 2; i++) {
    if (i > 0 && (len - i) % 3 == 0) out[j++] = ',';
    out[j++] = digits[i];
  }
  out[j] = 0;
}

// "$1,234.56"
static void fmt_cost(char *out, size_t n, uint32_t cents) {
  char grouped[24];
  fmt_grouped(grouped, sizeof grouped, cents / 100);
  snprintf(out, n, "$%s.%02lu", grouped, (unsigned long)(cents % 100));
}

// ---- generated big glyphs (bigfont.h) ----

static const BigGlyph *big_glyph(const BigFont &f, char c) {
  for (int i = 0; i < f.n; i++)
    if (f.glyphs[i].c == c) return &f.glyphs[i];
  return nullptr;
}

static int big_width(const BigFont &f, const char *s) {
  int w = 0, last_gap = 0;
  for (; *s; s++) {
    const BigGlyph *gl = big_glyph(f, *s);
    if (!gl) continue;
    w += gl->adv;
    last_gap = gl->adv - gl->w;
  }
  return w - last_gap;  // no tracking after the last glyph
}

static void big_draw(U8G2 &g, const BigFont &f, int x, int baseline, const char *s) {
  for (; *s; s++) {
    const BigGlyph *gl = big_glyph(f, *s);
    if (!gl) continue;
    g.drawXBM(x + gl->dx, baseline + gl->dy, gl->w, gl->h, gl->bits);
    x += gl->adv;
  }
}

// ---- sections ----

// 130 px reaches the battery's row, so it must stop short of the battery;
// 110 px starts below the battery and may run to the right margin.
static void draw_tokens(U8G2 &g, const Usage &u, const UiState &s) {
  struct Size { const BigFont *num, *unit; };
  static const Size sizes[] = {{&big_130, &unit_78}, {&big_110, &unit_66}};

  char num[24];
  const char *unit = "";
  if (u.valid) fmt_tokens(num, sizeof num, &unit, u.tokens[s.tab]);
  else strcpy(num, "--");

  const Size *sz = &sizes[0];
  int nw = 0, gap = 0;
  for (const Size &c : sizes) {
    sz = &c;
    gap = c.unit->cap / 8;
    nw = big_width(*c.num, num);
    int total = nw + (*unit ? gap + big_width(*c.unit, unit) : 0);
    bool reaches_battery = NUM_BASE - c.num->cap < BAT_Y + BAT_H + 6;
    int right = reaches_battery ? BAT_X - 16 : W - M;
    if (M + total <= right) break;
  }
  const int x = M - 2;  // glyph ink starts ~2 px in; align ink to the margin
  big_draw(g, *sz->num, x, NUM_BASE, num);
  if (*unit) big_draw(g, *sz->unit, x + nw + gap, NUM_BASE, unit);

  // Link drops are not shown; only data older than 15 min is flagged.
  char caption[64];
  snprintf(caption, sizeof caption, "tokens  /  %s%s", PERIOD_LABELS[s.tab],
           s.status == LINK_STALE ? "  /  not updated" : "");
  g.setFont(u8g2_font_helvR10_tr);
  g.drawStr(M, CAPTION_Y, caption);
}

static void draw_footer(U8G2 &g, const Usage &u, const UiState &s) {
  char cost[24] = "--", sessions[16] = "--";
  if (u.valid) {
    fmt_cost(cost, sizeof cost, u.cents[s.tab]);
    fmt_grouped(sessions, sizeof sessions, u.sessions[s.tab]);
  }
  g.setFont(u8g2_font_helvR10_tr);
  g.drawStr(M, FOOT_LABEL_Y, "API cost");
  g.drawStr(COL2, FOOT_LABEL_Y, "sessions");
  g.setFont(u8g2_font_logisoso26_tr);
  if (g.getStrWidth(cost) > COL2 - M - 12) g.setFont(u8g2_font_logisoso22_tr);
  g.drawStr(M, FOOT_Y, cost);
  g.setFont(u8g2_font_logisoso26_tr);
  g.drawStr(COL2, FOOT_Y, sessions);
}

void ui_draw(U8G2 &g, const Usage &u, const UiState &s) {
  g.clearBuffer();
  g.setDrawColor(1);
  g.setFontMode(1);

  draw_battery(g, s);
  draw_tokens(g, u, s);
  draw_footer(g, u, s);
  mascot_draw(g, W - M - MASCOT_W, FOOT_Y, s.mascot);

  g.sendBuffer();
}
