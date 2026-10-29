#include "Render.h"

#include <math.h>
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

const GFXfont *valueFontFor(int w, int h, const char *val, int unitW) {
  const GFXfont *cands[] = {font::NumXL, font::NumL, font::NumM, font::NumS};
  for (const GFXfont *f : cands) {
    int asc = ascent(f);
    if (asc > h) continue;
    if (textWidth(f, val) + unitW + 8 <= w) return f;
  }
  return font::NumS;
}

// Sparkline with the normal band drawn as dashed lines. Area is x,y,w,h.
void sparkline(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h, uint16_t fg, bool labels) {
  if (g.nspark < 2 || w < 40 || h < 18) return;
  float mn = g.spark[0], mx = g.spark[0];
  for (int i = 1; i < g.nspark; i++) {
    if (g.spark[i] < mn) mn = g.spark[i];
    if (g.spark[i] > mx) mx = g.spark[i];
  }
  if (g.hasRange) {
    if (g.lo < mn) mn = g.lo;
    if (g.hi > mx) mx = g.hi;
  }
  float span = mx - mn;
  if (span < 1e-6f) span = fabsf(mx) * 0.02f + 0.1f;
  mn -= span * 0.08f;
  mx += span * 0.08f;
  span = mx - mn;
  int labelW = labels && g.hasRange ? 56 : 0;
  int pw = w - labelW;
  auto Y = [&](float v) { return y + h - 1 - (int)lroundf((v - mn) / span * (h - 3)) - 1; };
  if (g.hasRange) {
    int ylo = Y(g.lo), yhi = Y(g.hi);
    dashedH(d, x, x + pw - 1, ylo, fg, 3, 5);
    dashedH(d, x, x + pw - 1, yhi, fg, 3, 5);
    if (labelW) {
      char b[16];
      Gauge t = g;
      t.has = true;
      t.value = g.hi;
      formatValue(t, b, sizeof b);
      drawText(d, font::Small, x + w, yhi + 5, b, fg, Right);
      t.value = g.lo;
      formatValue(t, b, sizeof b);
      drawText(d, font::Small, x + w, ylo + 5, b, fg, Right);
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
};

TileGeom geom(const Gauge &g, int w, int h) {
  TileGeom t;
  t.big = w >= 500 && h >= 300;
  t.pad = t.big ? 22 : 14;
  t.lf = (w >= 360 || t.big) ? font::Label : font::Body;
  t.ly = t.pad + ascent(t.lf);
  t.uf = t.big ? font::Head : (w >= 360 ? font::Label : font::Body);
  t.unitW = g.unit[0] ? textWidth(t.uf, g.unit) + 6 : 0;
  int availH = h - t.ly - t.pad - 14;
  t.wantSpark = g.nspark >= 2;
  t.valueMaxH = t.wantSpark ? availH * (t.big ? 58 : 52) / 100 : availH;
  return t;
}

const GFXfont *tileValueFont(const Gauge &g, int w, int h) {
  TileGeom t = geom(g, w, h);
  char val[16];
  formatValue(g, val, sizeof val);
  return valueFontFor(w - 2 * t.pad, t.valueMaxH, val, t.unitW);
}

void tile(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h, const GFXfont *vfShared) {
  bool alarm = g.status == Status::Alarm;
  uint16_t fg = alarm ? PAPER : INK, bg = alarm ? INK : PAPER;
  if (alarm) d.fillRoundRect(x, y, w, h, 10, INK);
  else if (g.status == Status::Warn) thickRoundRect(d, x, y, w, h, 10, 5, INK);
  else if (g.status == Status::Stale || g.status == Status::Missing) dashedRect(d, x, y, w, h, INK, 2);
  else thickRoundRect(d, x, y, w, h, 10, 2, INK);

  TileGeom t = geom(g, w, h);
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
    if (g.src[0] && w >= 360) snprintf(meta, sizeof meta, "%s · %s", g.src, age);
    else snprintf(meta, sizeof meta, "%s", age);
    rightW = drawText(d, font::Small, x + w - pad, ly, meta, fg, Right);
  }
  drawTextFit(d, lf, x + pad, ly, g.label, fg, w - 2 * pad - rightW - 10);

  // Value + unit.
  char val[16];
  formatValue(g, val, sizeof val);
  const GFXfont *uf = t.uf;
  bool wantSpark = t.wantSpark;
  const GFXfont *vf = vfShared ? vfShared : tileValueFont(g, w, h);
  int va = ascent(vf);
  int vy = ly + 14 + va;
  if (!wantSpark) vy = y + h / 2 + (ly - y) / 2 + va / 2 - 4;
  int vw = drawText(d, vf, x + pad - (big ? 4 : 2), vy, val, fg);
  if (g.unit[0]) drawText(d, uf, x + pad + vw + 4, vy, g.unit, fg);

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

void listRow(Adafruit_GFX &d, const Gauge &g, int x, int y, int w, int h) {
  bool alarm = g.status == Status::Alarm;
  uint16_t fg = alarm ? PAPER : INK, bg = alarm ? INK : PAPER;
  if (alarm) d.fillRoundRect(x, y + 1, w, h - 2, 6, INK);
  int base = y + h / 2 + 8;
  drawTextFit(d, font::Body, x + 12, base, g.label, fg, 250);
  if (g.nspark >= 2) {
    Gauge s = g;
    s.hasRange = g.hasRange;
    sparkline(d, s, x + 280, y + 6, 230, h - 12, fg, false);
  }
  char val[16];
  formatValue(g, val, sizeof val);
  const GFXfont *vf = h >= 44 ? font::NumS : font::Body;
  int vx = x + w - 110;
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

void footer(Adafruit_GFX &d, const Chrome &c) {
  int y = kH - kFooterH;
  uint16_t fg = INK;
  if (c.footerAlert) {
    d.fillRect(0, y, kW, kFooterH, INK);
    fg = PAPER;
  } else {
    d.fillRect(0, y, kW, 2, INK);
  }
  int rw = c.footerRight[0] ? textWidth(font::Small, c.footerRight) : 0;
  drawTextFit(d, font::Small, 14, y + 22, c.footerLeft, fg, kW - rw - 50);
  if (rw) drawText(d, font::Small, kW - 14, y + 22, c.footerRight, fg, Right);
}

}  // namespace

void gridFor(const Page &p, int &cols, int &rows) {
  int n = p.n;
  switch (p.layout) {
    case Layout::Single: cols = rows = 1; return;
    case Layout::Grid2x2: cols = 2; rows = 2; return;
    case Layout::Grid3x2: cols = 3; rows = 2; return;
    case Layout::List: cols = rows = 0; return;
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
  header(d, p, c);
  footer(d, c);

  if (p.n == 0) {
    drawText(d, font::Head, kW / 2, kH / 2 - 10, "No gauges on this page", INK, Center);
    drawText(d, font::Body, kW / 2, kH / 2 + 26, "Pick gauges in the panel settings or in Coeno.", INK, Center);
    return;
  }

  int cols, rows;
  gridFor(p, cols, rows);
  if (cols == 0) {
    int n = p.n;
    int rh = kBodyH / n;
    if (rh > 56) rh = 56;
    int y0 = kBodyY + (kBodyH - rh * n) / 2;
    for (int i = 0; i < n; i++) listRow(d, p.g[i], kBodyX, y0 + i * rh, kBodyW, rh);
    return;
  }
  int tw = (kBodyW - (cols - 1) * kGap) / cols;
  int th = (kBodyH - (rows - 1) * kGap) / rows;
  int n = p.n < cols * rows ? p.n : cols * rows;
  // One value size for the whole page, so tiles line up: the smallest any tile needs.
  const GFXfont *shared = nullptr;
  for (int i = 0; i < n; i++) {
    const GFXfont *f = tileValueFont(p.g[i], tw, th);
    if (!shared || ascent(f) < ascent(shared)) shared = f;
  }
  for (int i = 0; i < n; i++) {
    int cx = i % cols, cy = i / cols;
    tile(d, p.g[i], kBodyX + cx * (tw + kGap), kBodyY + cy * (th + kGap), tw, th, shared);
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
  drawTextFit(d, font::Body, x, y, "Add your Wi-Fi there, then Coeno,", INK, kW - x - 20);
  y += 28;
  drawTextFit(d, font::Body, x, y, "Apex or Hydros as the data source.", INK, kW - x - 20);
}

}  // namespace panel
