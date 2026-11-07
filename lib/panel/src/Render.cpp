#include "Render.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "Text.h"
extern "C" {
#include "qrcodegen.h"
}

namespace panel {

namespace {

constexpr int kHeaderH = 50;   // header / alarm banner
constexpr int kFooterH = 32;
constexpr int kBodyX = 8, kBodyY = kHeaderH + 8;
constexpr int kBodyW = kW - 16, kBodyH = kH - kHeaderH - kFooterH - 16;
// Gauge pages have no header (2026-11-06): the title and page dots sit in the
// footer, and the tiles get the whole height above it.
constexpr int kPageY = 8, kPageH = kH - kFooterH - 16;
constexpr int kGap = 8;

// ── small drawing helpers ────────────────────────────────────────────────

void dashedH(Adafruit_GFX &d, int x0, int x1, int y, uint16_t c, int on = 4, int off = 4) {
  for (int x = x0; x <= x1; x += on + off) d.drawFastHLine(x, y, (x + on > x1 ? x1 - x + 1 : on), c);
}

void dashedRect(Adafruit_GFX &d, int x, int y, int w, int h, uint16_t c, int t) {
  for (int i = 0; i < t; i++) {
    dashedH(d, x, x + w - 1, y + i, c, 10, 6);
    dashedH(d, x, x + w - 1, y + h - 1 - i, c, 10, 6);
    for (int yy = y; yy < y + h; yy += 16) {
      int len = yy + 10 > y + h ? y + h - yy : 10;
      d.drawFastVLine(x + i, yy, len, c);
      d.drawFastVLine(x + w - 1 - i, yy, len, c);
    }
  }
}

void thickRoundRect(Adafruit_GFX &d, int x, int y, int w, int h, int r, int t, uint16_t c) {
  for (int i = 0; i < t; i++) d.drawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, r > i ? r - i : 0, c);
}

void thickLine(Adafruit_GFX &d, int x0, int y0, int x1, int y1, uint16_t c) {
  d.drawLine(x0, y0, x1, y1, c);
  d.drawLine(x0, y0 + 1, x1, y1 + 1, c);
  d.drawLine(x0 + 1, y0, x1 + 1, y1, c);
}

// A small solid pill with text knocked out of it ("HIGH", "STALE 7 min").
// hollow = outline only. Returns the pill width.
int pill(Adafruit_GFX &d, int xRight, int yTop, const char *text, uint16_t fg, uint16_t bg, bool hollow) {
  const GFXfont *f = font::Small;
  int w = textWidth(f, text) + 16, h = 24;
  int x = xRight - w;
  if (hollow) {
    d.drawRoundRect(x, yTop, w, h, 6, fg);
    d.drawRoundRect(x + 1, yTop + 1, w - 2, h - 2, 5, fg);
    drawText(d, f, x + 8, yTop + 18, text, fg);
  } else {
    d.fillRoundRect(x, yTop, w, h, 6, fg);
    drawText(d, f, x + 8, yTop + 18, text, bg);
  }
  return w;
}

VFont valueFontFor(int w, int h, const char *val, int unitW) {
  const VFont cands[] = {font::HugeL, font::HugeM, font::HugeS, font::NumXL, font::NumLL, font::NumL, font::NumM, font::NumS};
  for (const VFont &f : cands) {
    int asc = ascent(f);
    if (asc > h) continue;
    if (textWidth(f, val) + unitW + 8 <= w) return f;
  }
  return font::NumS;
}

// Sparkline with the normal band drawn as dashed lines. Area is x,y,w,h.
// The smallest change a trend should make look big. Without it a graph scales
// 43.8-44.2 % humidity to full height and noise looks like a swing.
static float minSpan(const Gauge &g, float mid) {
  const char *u = g.unit;
  if (!strcmp(u, "°F")) return 1.0f;
  if (!strcmp(u, "°C")) return 0.5f;
  if (!strcmp(u, "%")) return 5.0f;
  if (!strcmp(u, "ppt") || !strcmp(u, "PSU")) return 0.5f;
  if (!strcmp(u, "mV")) return 20.0f;
  if (!strcmp(u, "dKH")) return 0.3f;
  if (!strcmp(u, "ppm")) return fmaxf(10.0f, fabsf(mid) * 0.03f);
  if (!strcmp(u, "pH") || strstr(g.label, "pH")) return 0.1f;
  return fmaxf(4.0f * powf(10.0f, -(float)g.dec), fabsf(mid) * 0.02f);
}

void sparkline(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h, uint16_t fg, bool labels) {
  if (g.nspark < 2 || w < 40 || h < 18) return;
  float mn = g.spark[0], mx = g.spark[0];
  for (int i = 1; i < g.nspark; i++) {
    if (g.spark[i] < mn) mn = g.spark[i];
    if (g.spark[i] > mx) mx = g.spark[i];
  }
  if (g.hasSparkRange) {
    // Coeno's y-range (already floored to a minimum span): data outside still
    // widens it, the normal band doesn't (it is drawn where it falls inside).
    if (g.sparkLo < mn) mn = g.sparkLo;
    if (g.sparkHi > mx) mx = g.sparkHi;
  } else {
    if (g.hasRange) {
      if (g.lo < mn) mn = g.lo;
      if (g.hi > mx) mx = g.hi;
    }
    float span = mx - mn, want = minSpan(g, (mn + mx) / 2);
    if (span < want) {  // widen around the middle
      float mid = (mn + mx) / 2;
      mn = mid - want / 2;
      mx = mid + want / 2;
      span = want;
    }
    mn -= span * 0.08f;
    mx += span * 0.08f;
  }
  float span = mx - mn;
  if (span < 1e-6f) span = 1e-6f;
  int labelW = labels && g.hasRange ? 56 : 0;
  int pw = w - labelW;
  auto Y = [&](float v) { return y + h - 1 - (int)lroundf((v - mn) / span * (h - 3)) - 1; };
  if (g.hasRange) {
    int ylo = Y(g.lo), yhi = Y(g.hi);
    char b[16];
    Gauge t = g;
    t.has = true;
    // Only the band lines that fall inside the plot (a spark range may be
    // narrower than the band).
    if (yhi >= y && yhi < y + h) {
      dashedH(d, x, x + pw - 1, yhi, fg, 3, 5);
      if (labelW) {
        t.value = g.hi;
        formatValue(t, b, sizeof b);
        drawText(d, font::Small, x + w, yhi + 5, b, fg, Right);
      }
    }
    if (ylo >= y && ylo < y + h) {
      dashedH(d, x, x + pw - 1, ylo, fg, 3, 5);
      if (labelW) {
        t.value = g.lo;
        formatValue(t, b, sizeof b);
        drawText(d, font::Small, x + w, ylo + 5, b, fg, Right);
      }
    }
  }
  int px = x, py = Y(g.spark[0]);
  for (int i = 1; i < g.nspark; i++) {
    int nx = x + (int)((long)i * (pw - 6) / (g.nspark - 1));
    int ny = Y(g.spark[i]);
    thickLine(d, px, py, nx, ny, fg);
    px = nx;
    py = ny;
  }
  d.fillCircle(px, py, 4, fg);
}

void noteFor(const Gauge &g, char *out, size_t n) {
  char age[16];
  switch (g.status) {
    case Status::Alarm:
    case Status::Warn:
      copyStr(out, n, g.note[0] ? g.note : (g.status == Status::Alarm ? "ALARM" : "CHECK"));
      break;
    case Status::Stale:
      formatAge(g.age_s, age, sizeof age);
      snprintf(out, n, "STALE %s", age);
      break;
    case Status::Missing:
      copyStr(out, n, "NO DATA");
      break;
    default:
      out[0] = 0;
  }
}

// ── one gauge in a tile ──────────────────────────────────────────────────

struct TileGeom {
  bool big;
  int pad;
  const GFXfont *lf, *uf;
  int ly;           // label baseline (relative to tile top)
  int valueMaxH;
  int unitW;
  bool wantSpark;
  bool graphs;      // View::Graphs: trend gets most of the tile
};

TileGeom geom(const Gauge &g, int w, int h, View view) {
  TileGeom t;
  t.big = w >= 500 && h >= 300;
  t.pad = t.big ? 22 : 14;
  t.lf = (w >= 360 || t.big) ? font::Label : font::Body;
  t.ly = t.pad + ascent(t.lf);
  t.uf = t.big ? font::Head : (w >= 360 ? font::Label : font::Body);
  t.unitW = g.unit[0] ? textWidth(t.uf, g.unit) + 6 : 0;
  int availH = h - t.ly - t.pad - 14;
  t.graphs = view == View::Graphs;
  // Graphs: keep the plot area even before there's a trend, so the page layout
  // doesn't jump when the first points arrive.
  t.wantSpark = view == View::Numbers ? false : t.graphs ? true : g.nspark >= 2;
  int pct = t.graphs ? 30 : t.big ? 58 : 52;
  t.valueMaxH = t.wantSpark ? availH * pct / 100 : availH;
  return t;
}

// Units grow with the value: 26 px up to ~100 px values, then 28, then 34.
static const GFXfont *unitFontFor(int valueAscent) {
  if (valueAscent >= 140) return font::NumS;
  if (valueAscent >= 100) return font::Head;
  return font::Label;
}

// Numbers view: the value as big as the tile allows, centred, with the unit
// beside it when that fits and under it otherwise. Values of 100 and up drop
// their decimals here (397, not 397.0).
static void numbersText(const Gauge &g, char *val, size_t n) {
  Gauge t = g;
  if (t.has && fabsf(t.value) >= 100.0f) t.dec = 0;
  formatValue(t, val, n);
}

static VFont numbersFont(const Gauge &g, int w, int h, const TileGeom &t) {
  char val[16];
  numbersText(g, val, sizeof val);
  // The unit sits in the lower-right corner; its line is always reserved
  // (unit or not) so values line up across a row.
  int areaW = w - 2 * t.pad, areaH = h - t.ly - t.pad - 6 - ascent(font::Label) - 8;
  // Room for a bigger unit too when the tile is big enough for a huge value.
  if (areaH > 160) areaH -= ascent(font::NumS) - ascent(font::Label);
  return valueFontFor(areaW, areaH, val, 0);
}

VFont tileValueFont(const Gauge &g, int w, int h, View view) {
  TileGeom t = geom(g, w, h, view);
  if (view == View::Numbers) return numbersFont(g, w, h, t);
  char val[16];
  formatValue(g, val, sizeof val);
  return valueFontFor(w - 2 * t.pad, t.valueMaxH, val, t.unitW);
}

// 24 h low / high of the trend, formatted like the value.
// The trend points are 30-min means, so the value on the tile can sit outside
// them: include it (pH 7.97 under "low 7.98" otherwise). Values of 100 and up
// lose their decimals here ("397 – 399", not "397.0 – 399.0" cut off).
static void sparkMinMax(const Gauge &g, char *lo, char *hi, size_t n) {
  float mn = g.spark[0], mx = g.spark[0];
  for (int i = 1; i < g.nspark; i++) {
    if (g.spark[i] < mn) mn = g.spark[i];
    if (g.spark[i] > mx) mx = g.spark[i];
  }
  if (g.has && !isnan(g.value)) {
    if (g.value < mn) mn = g.value;
    if (g.value > mx) mx = g.value;
  }
  Gauge t = g;
  t.has = true;
  if (fabsf(mx) >= 100.0f) t.dec = 0;
  t.value = mn;
  formatValue(t, lo, n);
  t.value = mx;
  formatValue(t, hi, n);
}

void tile(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h, VFont vfShared, View view) {
  bool alarm = g.status == Status::Alarm;
  uint16_t fg = alarm ? PAPER : INK, bg = alarm ? INK : PAPER;
  if (alarm) d.fillRoundRect(x, y, w, h, 10, INK);
  else if (g.status == Status::Warn) thickRoundRect(d, x, y, w, h, 10, 5, INK);
  else if (g.status == Status::Stale || g.status == Status::Missing) dashedRect(d, x, y, w, h, INK, 2);
  else thickRoundRect(d, x, y, w, h, 10, 2, INK);

  TileGeom t = geom(g, w, h, view);
  bool big = t.big;
  int pad = t.pad;
  const GFXfont *lf = t.lf;
  int la = ascent(lf);
  int ly = y + t.ly;

  // Right side of the label row: status pill, or source + age.
  char note[24];
  noteFor(g, note, sizeof note);
  int rightW = 0;
  if (note[0]) {
    rightW = pill(d, x + w - pad, ly - la - 2, note, fg, bg, g.status == Status::Stale || g.status == Status::Missing);
  } else {
    char meta[40], age[16];
    formatAge(g.age_s, age, sizeof age);
    if (g.age_s == kNoAge) snprintf(meta, sizeof meta, "%s", w >= 240 ? g.src : "");
    else if (g.src[0] && w >= 360) snprintf(meta, sizeof meta, "%s · %s", g.src, age);
    else snprintf(meta, sizeof meta, "%s", age);
    rightW = drawText(d, font::Small, x + w - pad, ly, meta, fg, Right);
  }
  drawTextFit(d, lf, x + pad, ly, g.label, fg, w - 2 * pad - rightW - 10);

  // Value + unit.
  char val[16];
  formatValue(g, val, sizeof val);
  const GFXfont *uf = t.uf;
  bool wantSpark = t.wantSpark;
  VFont vf = vfShared.ok() ? vfShared : tileValueFont(g, w, h, view);
  if (view == View::Numbers) {
    // The value centred between the label row and the unit line; the unit in
    // the lower-right corner, a size up from the other views. The unit line
    // is reserved even without a unit so values in a row share a baseline.
    numbersText(g, val, sizeof val);
    int va = ascent(vf);
    const GFXfont *nuf = unitFontFor(va);
    int ua = ascent(nuf);
    int top = ly + 6, bottom = y + h - pad - ua - 8;
    int base = top + (bottom - top - va) / 2 + va;
    drawText(d, vf, x + w / 2, base, val, fg, Center);
    if (g.unit[0]) drawText(d, nuf, x + w - pad, y + h - pad, g.unit, fg, Right);
    return;
  }
  int va = ascent(vf);
  int vy = ly + 14 + va;
  if (!wantSpark) vy = y + h / 2 + (ly - y) / 2 + va / 2 - 4;
  int vw = drawText(d, vf, x + pad - (big ? 4 : 2), vy, val, fg);
  if (g.unit[0]) drawText(d, va >= 140 ? unitFontFor(va) : uf, x + pad + vw + 6, vy, g.unit, fg);

  // Graphs view: the trend fills the rest, low / high of the last 24 h under it.
  if (wantSpark && t.graphs) {
    int sy = vy + (big ? 22 : 12);
    bool caption = h - (sy - y) >= 80;
    int sh = y + h - pad - sy - (caption ? 22 : 0);
    if (g.nspark >= 2) {
      sparkline(d, g, x + pad, sy, w - 2 * pad, sh, fg, w >= 300);
      if (caption) {
        char lo[16], hi[16], line[48];
        sparkMinMax(g, lo, hi, sizeof lo);
        snprintf(line, sizeof line, "24 h  %s – %s", lo, hi);
        drawTextFit(d, font::Small, x + pad, y + h - pad + 2, line, fg, w - 2 * pad);
      }
    } else {
      // Direct mode keeps one point per 30 min, so a trend needs about an hour.
      dashedRect(d, x + pad, sy, w - 2 * pad, sh > 20 ? sh : 20, fg, 1);
      drawTextFit(d, font::Small, x + w / 2, sy + sh / 2 + 6, "trend after about 1 h", fg, w - 2 * pad - 16,
                  Center);
    }
    return;
  }

  // Sparkline under the value.
  if (wantSpark) {
    int sy = vy + (big ? 26 : 14);
    int sh = y + h - pad - sy;
    if (big) sh -= 24;
    sparkline(d, g, x + pad, sy, w - 2 * pad, sh, fg, w >= 360);
    if (big) {
      char rng[64], lo[16], hi[16], age[16];
      Gauge t = g;
      t.has = true;
      t.value = g.lo;
      formatValue(t, lo, sizeof lo);
      t.value = g.hi;
      formatValue(t, hi, sizeof hi);
      formatAge(g.age_s, age, sizeof age);
      if (g.hasRange) snprintf(rng, sizeof rng, "normal %s – %s %s", lo, hi, g.unit);
      else rng[0] = 0;
      drawText(d, font::Small, x + pad, y + h - pad + 2, rng, fg);
      drawText(d, font::Small, x + w - pad, y + h - pad + 2, "last 24 h", fg, Right);
    }
  }
}

// ── list layout: one gauge per row ───────────────────────────────────────

void listRow(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h, View view) {
  bool alarm = g.status == Status::Alarm;
  uint16_t fg = alarm ? PAPER : INK, bg = alarm ? INK : PAPER;
  if (alarm) d.fillRoundRect(x, y + 1, w, h - 2, 6, INK);
  int base = y + h / 2 + 8;
  char val[16];
  formatValue(g, val, sizeof val);
  const GFXfont *vf = h >= 44 ? font::NumS : font::Body;
  int vx = x + w - 110;
  // Narrow rows (the takeover's right side): the label gets what the value leaves.
  int labelMax = vx - textWidth(vf, val) - (x + 12) - 14;
  if (labelMax > 250) labelMax = 250;
  drawTextFit(d, font::Body, x + 12, base, g.label, fg, labelMax);
  if (g.nspark >= 2 && view != View::Numbers && w >= 560) {
    Gauge s = g;
    s.hasRange = g.hasRange;
    sparkline(d, s, x + 280, y + 6, 230, h - 12, fg, false);
  }
  drawText(d, vf, vx, base + (h >= 44 ? 4 : 0), val, fg, Right);
  drawTextFit(d, font::Small, vx + 6, base, g.unit, fg, 50);
  char note[24];
  noteFor(g, note, sizeof note);
  if (note[0]) {
    const char *mark = g.status == Status::Stale ? "stale" : g.status == Status::Missing ? "--" : (g.note[0] == 'L' ? "▼" : g.note[0] == 'H' ? "▲" : "!");
    drawText(d, font::Body, x + w - 12, base, mark, fg, Right);
    if (g.status == Status::Warn) d.drawRoundRect(x, y + 1, w, h - 2, 6, INK), d.drawRoundRect(x + 1, y + 2, w - 2, h - 4, 5, INK);
  }
  (void)bg;
  if (!alarm) d.drawFastHLine(x + 8, y + h - 1, w - 16, INK);
}

// ── header / footer ──────────────────────────────────────────────────────

void header(Adafruit_GFX &d, const Page &p, const Chrome &c) {
  if (c.alarm[0]) {
    char text[80];
    if (c.alarmCount > 1) snprintf(text, sizeof text, "▲ %s   +%u more", c.alarm, (unsigned)(c.alarmCount - 1));
    else snprintf(text, sizeof text, "▲ %s", c.alarm);
    if (c.alarmAcked) {
      d.fillRect(0, 0, kW, kHeaderH, PAPER);
      thickRoundRect(d, 2, 2, kW - 4, kHeaderH - 4, 8, 3, INK);
      drawTextFit(d, font::Head, 18, 36, text, INK, kW - 140);
      drawText(d, font::Small, kW - 18, 32, "acknowledged", INK, Right);
    } else {
      d.fillRect(0, 0, kW, kHeaderH, INK);
      drawTextFit(d, font::Head, 18, 36, text, PAPER, kW - 120);
      if (c.clock[0]) drawText(d, font::Head, kW - 16, 36, c.clock, PAPER, Right);
    }
    return;
  }
  int right = kW - 16;
  if (c.clock[0]) right -= drawText(d, font::Head, right, 36, c.clock, INK, Right) + 18;
  if (c.pages > 1) {
    for (int i = c.pages - 1; i >= 0; i--) {
      int cx = right - 6;
      if (i == c.page) d.fillCircle(cx, 26, 6, INK);
      else {
        d.drawCircle(cx, 26, 6, INK);
        d.drawCircle(cx, 26, 5, INK);
      }
      right -= 20;
    }
    right -= 8;
  }
  drawTextFit(d, font::Head, 16, 36, p.title[0] ? p.title : "Reef", INK, right - 24);
  d.fillRect(0, kHeaderH - 2, kW, 2, INK);
}

static int footerIcons(Adafruit_GFX &d, const Chrome &c, int xr, int base, uint16_t fg);

void footer(Adafruit_GFX &d, const Chrome &c) {
  int y = kH - kFooterH;
  uint16_t fg = INK;
  if (c.footerAlert) {
    d.fillRect(0, y, kW, kFooterH, INK);
    fg = PAPER;
  } else {
    d.fillRect(0, y, kW, 2, INK);
  }
  int ix = footerIcons(d, c, kW - 14, y + 22, fg);
  int rw = c.footerRight[0] ? textWidth(font::Small, c.footerRight) : 0;
  if (rw) drawText(d, font::Small, ix - 4, y + 22, c.footerRight, fg, Right);
  drawTextFit(d, font::Small, 14, y + 22, c.footerLeft, fg, ix - (rw ? rw + 16 : 0) - 50);
}

// Footer icons, right-aligned at xr (their right edge), baseline y. Returns
// the x where they start.
static int footerIcons(Adafruit_GFX &d, const Chrome &c, int xr, int base, uint16_t fg) {
  int x = xr;
  if (c.battery >= 0) {
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", c.battery);
    x -= drawText(d, font::Small, x, base, pct, fg, Right) + 6;
    // body 26 x 13 with a nub, fill by charge
    int bw = 26, bh = 13, by = base - 13;
    x -= 3;
    d.fillRect(x, by + 4, 3, 5, fg);  // nub
    x -= bw;
    d.drawRect(x, by, bw, bh, fg);
    d.drawRect(x + 1, by + 1, bw - 2, bh - 2, fg);
    int fw = (bw - 6) * (c.battery > 100 ? 100 : c.battery) / 100;
    if (fw > 0) d.fillRect(x + 3, by + 3, fw, bh - 6, fg);
    x -= 14;
  }
  if (c.wifi >= 0) {
    // The usual fan: a dot and three 90° arcs. Arcs the signal reaches are
    // solid and 2 px; the rest dotted, so "2 of 4" reads at a glance.
    int cx = x - 13, cy = base - 1;
    d.fillCircle(cx, cy - 1, 2, fg);
    for (int k = 1; k <= 3; k++) {
      int r = 4 + k * 5;
      bool strong = c.wifi >= k + 1;
      int steps = r * 3;
      for (int i = 0; i <= steps; i++) {
        if (!strong && (i % 3)) continue;
        float a = (45.0f + 90.0f * i / steps) * 3.14159265f / 180.0f;
        int px = cx + (int)lroundf(cosf(a) * r), py = cy - (int)lroundf(sinf(a) * r);
        d.drawPixel(px, py, fg);
        if (strong) {
          d.drawPixel(cx + (int)lroundf(cosf(a) * (r - 1)), cy - (int)lroundf(sinf(a) * (r - 1)), fg);
        }
      }
    }
    if (c.wifi == 0)  // offline: struck through
      for (int t = 0; t < 2; t++) d.drawLine(cx - 12, cy - 17 + t, cx + 12, cy + 1 + t, fg);
    x = cx - 22;
  }
  return x;
}

// Gauge pages: title and page dots on the left of the footer, then the
// status; Wi-Fi / battery on the right. An unacknowledged alarm turns the
// footer black with the alarm text (the takeover tile shows the reading).
void pageFooter(Adafruit_GFX &d, const Page &p, const Chrome &c) {
  int y = kH - kFooterH;
  bool alarm = c.alarm[0] && !c.alarmAcked;
  bool inv = alarm || c.footerAlert;
  uint16_t fg = inv ? PAPER : INK;
  if (inv) d.fillRect(0, y, kW, kFooterH, INK);
  else d.fillRect(0, y, kW, 2, INK);
  int base = y + 23;
  int x = 14;
  x += drawText(d, font::Body, x, base + 1, p.title[0] ? p.title : "Reef", fg) + 10;
  if (c.pages > 1) {
    for (int i = 0; i < c.pages; i++) {
      int cx = x + 5;
      if (i == c.page) d.fillCircle(cx, y + 16, 5, fg);
      else d.drawCircle(cx, y + 16, 5, fg);
      x += 16;
    }
    x += 6;
  }
  char left[96];
  if (c.alarm[0]) {
    if (c.alarmCount > 1)
      snprintf(left, sizeof left, "▲ %s +%u more%s", c.alarm, (unsigned)(c.alarmCount - 1), c.alarmAcked ? " · acknowledged" : "");
    else snprintf(left, sizeof left, "▲ %s%s", c.alarm, c.alarmAcked ? " · acknowledged" : "");
  } else {
    snprintf(left, sizeof left, "%s", c.footerLeft);
  }
  int ix = footerIcons(d, c, kW - 14, base, fg);
  int rw = c.footerRight[0] ? textWidth(font::Small, c.footerRight) : 0;
  if (rw) drawText(d, font::Small, ix - 4, base, c.footerRight, fg, Right);
  int rightEdge = ix - (rw ? rw + 16 : 0);
  drawText(d, font::Small, x, base, "·", fg);
  drawTextFit(d, font::Small, x + 14, base, left, fg, rightEdge - x - 30);
}

// Alarm takeover: one or two gauges in alarm get the left half of the page,
// big and with their trend; everything else is a compact list on the right.
// Returns false (draw the normal grid) when there's no alarm, more than two,
// or nothing else to show.
bool takeover(Adafruit_GFX &d, const Page &p) {
  int na = 0, ia[2] = {-1, -1};
  for (int i = 0; i < p.n; i++)
    if (p.g[i].status == Status::Alarm) {
      if (na < 2) ia[na] = i;
      na++;
    }
  if (na == 0 || na > 2 || p.n < 2) return false;
  int lw = kBodyW * 52 / 100;
  int th = na == 1 ? kPageH : (kPageH - kGap) / 2;
  for (int k = 0; k < na; k++)
    tile(d, p.g[ia[k]], kBodyX, kPageY + k * (th + kGap), lw, th, nullptr, View::Graphs);
  int rx = kBodyX + lw + kGap, rw = kBodyW - lw - kGap;
  int rest = p.n - na;
  int rh = kPageH / rest;
  if (rh > 74) rh = 74;
  int y0 = kPageY + (kPageH - rh * rest) / 2;
  int r = 0;
  for (int i = 0; i < p.n; i++) {
    if (i == ia[0] || i == ia[1]) continue;
    listRow(d, p.g[i], rx, y0 + r * rh, rw, rh, View::Numbers);
    r++;
  }
  return true;
}

// ── timeline page: one lane per gauge, time across ───────────────────────

// "2 PM" / "14:00" for an axis tick, "2:30 PM" / "14:30" for a time in text;
// relative ("-6 h", "+30 min") when the page has no clock.
void clockLabel(const Page &p, int t, bool axis, char *out, size_t n) {
  if (p.clock < 0) {
    if (t == 0) { copyStr(out, n, "now"); return; }
    int a = t < 0 ? -t : t;
    if (a % 60 == 0) snprintf(out, n, "%c%d h", t < 0 ? '-' : '+', a / 60);
    else if (a > 90) snprintf(out, n, "%c%d:%02d", t < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(out, n, "%c%d min", t < 0 ? '-' : '+', a);
    return;
  }
  int m = ((p.clock + t) % 1440 + 1440) % 1440;
  int h = m / 60, mi = m % 60;
  if (!p.h12) {
    snprintf(out, n, "%d:%02d", h, mi);
    return;
  }
  int h12 = h % 12 ? h % 12 : 12;
  const char *ap = h < 12 ? "AM" : "PM";
  if (axis && mi == 0) snprintf(out, n, "%d %s", h12, ap);
  else snprintf(out, n, "%d:%02d %s", h12, mi, ap);
}

int tickStep(int span) {
  if (span <= 180) return 30;
  if (span <= 480) return 60;
  if (span <= 960) return 120;
  if (span <= 1500) return 180;
  return 360;
}

// What the lane's label cell says under the name: "80 %", "ON · off 2:30 PM",
// "next 2:00 PM".
void laneState(const Page &p, const Gauge &g, char *out, size_t n) {
  char v[24], tm[16];
  out[0] = 0;
  switch (g.lane) {
    case Lane::Level:
    case Lane::Step: {
      Gauge s = g;
      if (!s.has) {
        s.value = laneValueAt(g, 0);
        s.has = !isnan(s.value);
      }
      formatValue(s, v, sizeof v);
      if (s.has) snprintf(out, n, g.unit[0] ? "%s %s" : "%s", v, g.unit);
      break;
    }
    case Lane::OnOff: {
      float now = g.has ? g.value : laneValueAt(g, 0);
      if (isnan(now)) break;
      bool on = now > 0.5f;
      for (int i = 0; i < g.nspark; i++) {
        if (g.t[i] > 0 && g.t[i] < p.to && (g.spark[i] > 0.5f) != on) {
          clockLabel(p, g.t[i], false, tm, sizeof tm);
          snprintf(out, n, "%s · %s %s", on ? "ON" : "OFF", on ? "off" : "on", tm);
          return;
        }
      }
      // Nothing scheduled ahead (direct mode: the panel only saw the past):
      // say since when instead. "by hand" from g.note.
      for (int i = g.nspark - 1; i > 0; i--) {
        if (g.t[i] <= 0 && g.t[i] > p.from && (g.spark[i] > 0.5f) == on && (g.spark[i - 1] > 0.5f) != on) {
          clockLabel(p, g.t[i], false, tm, sizeof tm);
          // "ON since 7:15 PM"; forced by hand just "ON by hand" (both don't fit)
          if (g.note[0]) snprintf(out, n, "%s %s", on ? "ON" : "OFF", g.note);
          else snprintf(out, n, "%s since %s", on ? "ON" : "OFF", tm);
          return;
        }
      }
      snprintf(out, n, "%s%s%s", on ? "ON" : "OFF", g.note[0] ? " " : "", g.note);
      break;
    }
    case Lane::Events: {
      for (int i = 0; i < g.nspark; i++) {
        if (g.t[i] > 0) {
          clockLabel(p, g.t[i], false, tm, sizeof tm);
          snprintf(out, n, "next %s", tm);
          return;
        }
      }
      if (g.nspark) {
        clockLabel(p, g.t[g.nspark - 1], false, tm, sizeof tm);
        snprintf(out, n, "last %s", tm);
      }
      break;
    }
  }
}

void timeline(Adafruit_GFX &d, const Page &p) {
  // Label column: 176 px, wider (to 250) when names need it, e.g. Hydros
  // outputs like "1RX10D1-Part C".
  int labelW = 176;
  const int axisH = 30;
  for (int i = 0; i < p.n; i++) {
    int w = textWidth(font::Body, p.g[i].label) + 34;
    if (w > labelW) labelW = w;
  }
  if (labelW > 250) labelW = 250;
  const int x0 = kBodyX + labelW, x1 = kBodyX + kBodyW - 6, pw = x1 - x0;
  const int span = p.to - p.from;
  auto X = [&](float t) { return x0 + (int)lroundf((t - p.from) * pw / (float)span); };
  auto T = [&](int x) { return p.from + (x - x0) * (float)span / pw; };

  int n = p.n;
  int laneH = (kPageH - axisH) / n;
  if (laneH > 86) laneH = 86;
  int top = kPageY + 2 + (kPageH - axisH - laneH * n) / 2;
  int bot = top + laneH * n;  // axis line

  // Grid: dotted verticals at each tick.
  int step = tickStep(span);
  int base = p.clock >= 0 ? p.clock : 0;
  int first = p.from + ((step - ((base + p.from) % step + step) % step) % step);
  for (int t = first; t <= p.to; t += step)
    for (int y = top; y < bot; y += 4) d.drawPixel(X(t), y, INK);

  for (int i = 0; i < n; i++) {
    const Gauge &g = p.g[i];
    int ly = top + i * laneH;
    bool alarm = g.status == Status::Alarm;
    uint16_t fg = alarm ? PAPER : INK;
    if (alarm) d.fillRoundRect(kBodyX, ly + 3, labelW - 12, laneH - 6, 6, INK);
    else if (g.status == Status::Warn) thickRoundRect(d, kBodyX, ly + 3, labelW - 12, laneH - 6, 6, 3, INK);
    // Label cell: name, and the state under it when there's room.
    char st[40];
    laneState(p, g, st, sizeof st);
    bool two = laneH >= 46 && st[0];
    int nameY = two ? ly + laneH / 2 - 2 : ly + laneH / 2 + 8;
    drawTextFit(d, font::Body, kBodyX + 10, nameY, g.label, fg, labelW - 30);
    if (two) drawTextFit(d, font::Small, kBodyX + 10, nameY + 22, st, fg, labelW - 30);
    if (i < n - 1) dashedH(d, kBodyX + 6, x1, ly + laneH - 1, INK, 2, 4);

    // Plot area of this lane.
    int yt = ly + 8, yb = ly + laneH - 8;
    if (g.nspark == 0) {
      // Coeno sends the schedule; a past-only page (direct mode) has only
      // seen the time since the panel started.
      drawText(d, font::Small, x0 + 10, ly + laneH / 2 + 6,
               p.to > 0 ? "no schedule" : g.lane == Lane::Events ? "no doses seen yet" : "no change seen yet", INK);
      continue;
    }
    switch (g.lane) {
      case Lane::Level:
      case Lane::Step: {
        float lo = 0, hi = 0;
        if (g.hasRange) {
          lo = g.lo;
          hi = g.hi;
        } else {
          for (int k = 0; k < g.nspark; k++) {
            if (!isnan(g.spark[k]) && g.spark[k] > hi) hi = g.spark[k];
            if (!isnan(g.spark[k]) && g.spark[k] < lo) lo = g.spark[k];
          }
          if (!strcmp(g.unit, "%") && hi <= 100) hi = 100;
        }
        if (hi - lo < 1e-6f) hi = lo + 1;
        auto Y = [&](float v) {
          if (v < lo) v = lo;
          if (v > hi) v = hi;
          return yb - (int)lroundf((v - lo) / (hi - lo) * (yb - yt));
        };
        int xs = X(g.t[0] > p.from ? g.t[0] : p.from);
        int xe = X(p.to);
        int nowX = X(0), py = -1;
        for (int x = xs; x <= xe; x++) {
          float v = laneValueAt(g, T(x));
          if (isnan(v)) { py = -1; continue; }
          int y = Y(v);
          bool past = x <= nowX;
          if (past && (x - x0) % 3 == 0 && y < yb) d.drawFastVLine(x, y, yb - y + 1, INK);  // hatched = happened
          if (py >= 0 && py != y) d.drawFastVLine(x, py < y ? py : y, abs(py - y) + 1, INK);
          d.drawFastVLine(x, y, past ? 3 : 2, INK);
          py = y;
        }
        if (xs <= nowX) d.drawFastHLine(xs, yb, (nowX < xe ? nowX : xe) - xs + 1, INK);
        break;
      }
      case Lane::OnOff: {
        int bh = (yb - yt) * 2 / 3, by = (yt + yb) / 2 - bh / 2;
        for (int k = 0; k < g.nspark; k++) {
          if (!(g.spark[k] > 0.5f)) continue;
          int a = g.t[k], b = k + 1 < g.nspark ? g.t[k + 1] : p.to;
          if (b <= p.from || a >= p.to) continue;
          if (a < p.from) a = p.from;
          if (b > p.to) b = p.to;
          int xa = X(a), xb = X(b);
          if (xb - xa < 3) xb = xa + 3;  // a short run still shows
          int xn = X(0);
          // happened: solid; scheduled: outline
          if (a < 0) d.fillRect(xa, by, (b <= 0 ? xb : xn) - xa, bh, INK);
          if (b > 0) {
            int s = a > 0 ? xa : xn;
            d.drawRect(s, by, xb - s, bh, INK);
            d.drawRect(s + 1, by + 1, xb - s - 2, bh - 2, INK);
          }
        }
        break;
      }
      case Lane::Events: {
        int cy = (yt + yb) / 2 + (laneH >= 50 ? 8 : 0);
        int lastLabelEnd = -1000;
        for (int k = 0; k < g.nspark; k++) {
          if (g.t[k] < p.from || g.t[k] > p.to) continue;
          int x = X(g.t[k]);
          if (g.t[k] <= 0) {
            d.fillCircle(x, cy, 6, INK);
          } else {
            d.drawCircle(x, cy, 6, INK);
            d.drawCircle(x, cy, 5, INK);
          }
          d.drawFastVLine(x, cy + 6, yb - cy - 4, INK);
          if (laneH >= 50 && !isnan(g.spark[k])) {
            Gauge s = g;
            s.has = true;
            s.value = g.spark[k];
            char v[16];
            formatValue(s, v, sizeof v);
            int w = textWidth(font::Small, v);
            if (x - w / 2 > lastLabelEnd + 4) {
              drawText(d, font::Small, x, cy - 11, v, INK, Center);
              lastLabelEnd = x + w / 2;
            }
          }
        }
        break;
      }
    }
  }

  // Axis: line, tick labels, "now".
  d.fillRect(x0, bot, pw + 1, 2, INK);
  int nowX = X(0);
  char b[16];
  clockLabel(p, 0, false, b, sizeof b);
  int nw = textWidth(font::Small, b) + 14;
  for (int t = first; t <= p.to; t += step) {
    int x = X(t);
    d.drawFastVLine(x, bot, 6, INK);
    clockLabel(p, t, true, b, sizeof b);
    int w = textWidth(font::Small, b);
    int lx = x - w / 2;
    if (lx < x0 - 20 || lx + w > kW - 4) continue;
    if (lx + w > nowX - nw / 2 - 4 && lx < nowX + nw / 2 + 4) continue;  // the "now" tag wins
    drawText(d, font::Small, x, bot + 24, b, INK, Center);
  }
  if (p.from < 0 && p.to > 0) {
    d.fillRect(nowX - 1, top - 2, 3, bot - top + 2, INK);
    clockLabel(p, 0, false, b, sizeof b);
    int w = textWidth(font::Small, b) + 14;
    int px = nowX - w / 2;
    if (px + w > kW - 2) px = kW - 2 - w;
    d.fillRoundRect(px, bot + 4, w, 24, 6, INK);
    drawText(d, font::Small, px + 7, bot + 22, b, PAPER);
  }
}

}  // namespace

void gridFor(const Page &p, int &cols, int &rows) {
  int n = p.n;
  switch (p.layout) {
    case Layout::Single: cols = rows = 1; return;
    case Layout::Grid2x2: cols = 2; rows = 2; return;
    case Layout::Grid3x2: cols = 3; rows = 2; return;
    case Layout::List:
    case Layout::Timeline: cols = rows = 0; return;
    default: break;
  }
  if (n <= 1) { cols = rows = 1; }
  else if (n == 2) { cols = 2; rows = 1; }
  else if (n == 3) { cols = 3; rows = 1; }
  else if (n == 4) { cols = 2; rows = 2; }
  else if (n <= 6) { cols = 3; rows = 2; }
  else { cols = rows = 0; }
}

void render(Adafruit_GFX &d, const Page &p, const Chrome &c) {
  d.fillScreen(PAPER);
  pageFooter(d, p, c);

  if (p.n == 0) {
    drawText(d, font::Head, kW / 2, kH / 2 - 10, "No gauges on this page", INK, Center);
    drawText(d, font::Body, kW / 2, kH / 2 + 26, "Pick gauges in the panel settings or in Coeno.", INK, Center);
    return;
  }

  if (p.layout == Layout::Timeline) {
    timeline(d, p);
    return;
  }
  if ((p.layout == Layout::Auto || p.layout == Layout::Grid2x2 || p.layout == Layout::Grid3x2) && takeover(d, p)) return;
  int cols, rows;
  gridFor(p, cols, rows);
  if (cols == 0) {
    int n = p.n;
    int rh = kPageH / n;
    if (rh > 56) rh = 56;
    int y0 = kPageY + (kPageH - rh * n) / 2;
    for (int i = 0; i < n; i++) listRow(d, p.g[i], kBodyX, y0 + i * rh, kBodyW, rh, p.view);
    return;
  }
  int tw = (kBodyW - (cols - 1) * kGap) / cols;
  int th = (kPageH - (rows - 1) * kGap) / rows;
  int n = p.n < cols * rows ? p.n : cols * rows;
  // One value size for the whole page, so tiles line up: the smallest any tile needs.
  VFont shared;
  for (int i = 0; i < n; i++) {
    VFont f = tileValueFont(p.g[i], tw, th, p.view);
    if (!shared.ok() || ascent(f) < ascent(shared)) shared = f;
  }
  for (int i = 0; i < n; i++) {
    int cx = i % cols, cy = i / cols;
    tile(d, p.g[i], kBodyX + cx * (tw + kGap), kPageY + cy * (th + kGap), tw, th, shared, p.view);
  }
}

void renderMessage(Adafruit_GFX &d, const char *title, const char *body, const char *big, const Chrome &c) {
  d.fillScreen(PAPER);
  Page p;
  PANEL_COPY(p.title, title);
  Chrome hc = c;
  hc.pages = 1;
  hc.alarm[0] = 0;
  header(d, p, hc);
  footer(d, c);
  int y = 130;
  if (big && big[0]) {
    drawText(d, font::NumS, kW / 2, y, big, INK, Center);
    y += 60;
  }
  char line[96];
  const char *s = body ? body : "";
  while (*s) {
    const char *e = strchr(s, '\n');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, s, n);
    line[n] = 0;
    drawTextFit(d, font::Body, kW / 2, y, line, INK, kW - 60, Center);
    y += 34;
    s += n;
    if (*s == '\n') s++;
  }
}

void renderSetup(Adafruit_GFX &d, const SetupInfo &s, const Chrome &c) {
  d.fillScreen(PAPER);
  Page p;
  PANEL_COPY(p.title, "Set up this panel");
  Chrome hc = c;
  hc.pages = 1;
  hc.alarm[0] = 0;
  header(d, p, hc);
  footer(d, c);

  // QR code that joins the setup Wi-Fi (phones then open the page by themselves).
  char wifi[160];
  snprintf(wifi, sizeof wifi, "WIFI:T:WPA;S:%s;P:%s;;", s.apName, s.apPass);
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
  int qx = 40, qy = 80, box = 300;
  if (qrcodegen_encodeText(wifi, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true)) {
    int size = qrcodegen_getSize(qr);
    int m = box / (size + 4);
    int off = (box - m * size) / 2;
    for (int yy = 0; yy < size; yy++)
      for (int xx = 0; xx < size; xx++)
        if (qrcodegen_getModule(qr, xx, yy)) d.fillRect(qx + off + xx * m, qy + off + yy * m, m, m, INK);
  }
  drawText(d, font::Small, qx + box / 2, qy + box + 22, "Scan to join", INK, Center);

  int x = 380, y = 104;
  if (s.reason && s.reason[0]) {
    drawTextFit(d, font::Body, x, y, s.reason, INK, kW - x - 20);
    y += 46;
  }
  drawText(d, font::Label, x, y, "1. Join this Wi-Fi", INK);
  y += 40;
  drawTextFit(d, font::NumS, x + 24, y, s.apName, INK, kW - x - 40);
  y += 34;
  char pw[64];
  snprintf(pw, sizeof pw, "password  %s", s.apPass);
  drawText(d, font::Body, x + 24, y, pw, INK);
  y += 58;
  drawText(d, font::Label, x, y, "2. Open", INK);
  y += 40;
  drawTextFit(d, font::NumS, x + 24, y, s.url, INK, kW - x - 40);
  y += 46;
  drawTextFit(d, font::Body, x, y, "Add your Wi-Fi and the data source.", INK, kW - x - 20);
  y += 28;
  drawTextFit(d, font::Body, x, y, "Settings login: admin + this password.", INK, kW - x - 20);
}

void renderSystem(Adafruit_GFX &d, const SystemInfo &s, const Chrome &c) {
  d.fillScreen(PAPER);
  Page p;
  PANEL_COPY(p.title, "System");
  Chrome hc = c;
  hc.pages = 1;
  hc.alarm[0] = 0;
  header(d, p, hc);
  footer(d, c);

  // Left: the update card.
  const int cx = kBodyX, cy = kBodyY + 4, cw = 452, ch = kBodyH - 8;
  const int px = cx + 24;
  bool inv = s.state == SystemInfo::Available;
  uint16_t fg = inv ? PAPER : INK, bg = inv ? INK : PAPER;
  if (inv) d.fillRoundRect(cx, cy, cw, ch, 12, INK);
  else if (s.state == SystemInfo::Failed || s.state == SystemInfo::Installing) thickRoundRect(d, cx, cy, cw, ch, 12, 5, INK);
  else thickRoundRect(d, cx, cy, cw, ch, 12, 2, INK);

  const char *title = "Firmware";
  switch (s.state) {
    case SystemInfo::Checking: title = "Checking for updates…"; break;
    case SystemInfo::UpToDate: title = "Up to date"; break;
    case SystemInfo::Available: title = "Update available"; break;
    case SystemInfo::Installing: title = "Updating"; break;
    case SystemInfo::Failed: title = "Update failed"; break;
    default: break;
  }
  int y = cy + 46;
  drawTextFit(d, font::Head, px, y, title, fg, cw - 48);

  // Installed / available build numbers.
  y += 30;
  char b[24];
  drawText(d, font::Small, px, y, "INSTALLED", fg);
  bool two = s.availBuild > 0 && (s.state == SystemInfo::Available || s.state == SystemInfo::Installing);
  if (two) {
    char from[32];
    snprintf(from, sizeof from, "NEW · %s", s.availFrom);
    drawTextFit(d, font::Small, px + 210, y, from, fg, cw - 240);
  }
  y += 60;
  snprintf(b, sizeof b, "%ld", s.build);
  drawText(d, font::NumM, px - 2, y, b, fg);
  if (two) {
    // a fat arrow made of a bar and a triangle (no arrow glyph in the fonts)
    int ax = px + 150, ay = y - 22;
    d.fillRect(ax, ay - 4, 22, 9, fg);
    d.fillTriangle(ax + 20, ay - 14, ax + 20, ay + 14, ax + 40, ay, fg);
    snprintf(b, sizeof b, "%ld", s.availBuild);
    drawText(d, font::NumM, px + 208, y, b, fg);
  }
  y += 26;
  drawTextFit(d, font::Small, px, y, s.version, fg, cw - 48);

  // State body.
  y += 40;
  if (s.state == SystemInfo::Installing) {
    int bw = cw - 48, bh = 30;
    thickRoundRect(d, px, y, bw, bh, 8, 3, INK);
    if (s.pct >= 0) {
      int fw = (bw - 12) * (s.pct > 100 ? 100 : s.pct) / 100;
      if (fw > 0) d.fillRoundRect(px + 6, y + 6, fw, bh - 12, 4, INK);
      snprintf(b, sizeof b, "%d %%", s.pct);
    } else {
      copyStr(b, sizeof b, "starting…");
    }
    y += bh + 30;
    char line[64];
    snprintf(line, sizeof line, "Downloading from %s · %s", s.availFrom[0] ? s.availFrom : "Coeno", b);
    drawTextFit(d, font::Body, px, y, line, INK, cw - 48);
    y += 28;
    drawTextFit(d, font::Body, px, y, "Don't unplug. It restarts by itself.", INK, cw - 48);
  } else {
    if (s.detail[0]) {
      drawTextFit(d, font::Body, px, y, s.detail, fg, cw - 48);
      y += 28;
    }
    if (s.checked[0]) drawTextFit(d, font::Small, px, y, s.checked, fg, cw - 48);
  }

  // Button hint along the bottom of the card.
  if (s.hint[0] && s.state != SystemInfo::Installing) {
    int hy = cy + ch - 56;
    int hw = cw - 48;
    if (inv) {
      d.fillRoundRect(px, hy, hw, 38, 19, PAPER);
      drawTextFit(d, font::Label, px + hw / 2, hy + 28, s.hint, INK, hw - 24, Center);
    } else {
      thickRoundRect(d, px, hy, hw, 38, 19, 2, INK);
      const GFXfont *hf = textWidth(font::Body, s.hint) <= hw - 24 ? font::Body : font::Small;
      drawTextFit(d, hf, px + hw / 2, hy + (hf == font::Body ? 26 : 25), s.hint, INK, hw - 24, Center);
    }
  }
  (void)bg;

  // Right: details.
  int rx = cx + cw + 22, rw = kW - rx - 14;
  int ry = cy + 30;
  for (int i = 0; i < s.nrows && i < 8; i++) {
    if (!s.keys[i] || !s.vals[i]) continue;
    drawTextFit(d, font::Small, rx, ry, s.keys[i], INK, rw);
    drawTextFit(d, font::Body, rx, ry + 24, s.vals[i], INK, rw);
    ry += 50;
    if (ry > kH - kFooterH - 20) break;
  }
}

}  // namespace panel
