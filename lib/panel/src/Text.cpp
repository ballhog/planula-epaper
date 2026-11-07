#include "Text.h"

#include <string.h>

#include "fonts/InterBody.h"
#include "fonts/InterHead.h"
#include "fonts/InterLabel.h"
#include "fonts/InterNumL.h"
#include "fonts/InterNumLL.h"
#include "fonts/InterNumM.h"
#include "fonts/InterNumS.h"
#include "fonts/InterNumXL.h"
#include "fonts/InterHugeL.h"
#include "fonts/InterHugeM.h"
#include "fonts/InterHugeS.h"
#include "fonts/InterSmall.h"

#ifndef pgm_read_byte
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#endif

namespace panel {

namespace font {
const GFXfont *Small = &InterSmall;
const GFXfont *Body = &InterBody;
const GFXfont *Label = &InterLabel;
const GFXfont *Head = &InterHead;
const GFXfont *NumS = &InterNumS;
const GFXfont *NumM = &InterNumM;
const GFXfont *NumL = &InterNumL;
const GFXfont *NumLL = &InterNumLL;
const GFXfont *NumXL = &InterNumXL;
const BigFont *HugeS = &InterHugeS;
const BigFont *HugeM = &InterHugeM;
const BigFont *HugeL = &InterHugeL;
}  // namespace font

namespace {

struct Extra {
  uint32_t cp;
  char code;
};
// Must match EXTRAS in tools/mkfonts.py.
const Extra kExtras[] = {
    {0x00B0, 0x7F},  // °
    {0x00B5, (char)0x80}, {0x03BC, (char)0x80},  // µ (micro sign and Greek mu)
    {0x00B3, (char)0x81},  // ³
    {0x2082, (char)0x82},  // ₂
    {0x2014, (char)0x83}, {0x2013, (char)0x83},  // — (and – drawn the same)
    {0x00B7, (char)0x84},  // ·
    {0x25B2, (char)0x85},  // ▲
    {0x25BC, (char)0x86},  // ▼
};

const GFXglyph *glyphFor(const GFXfont *f, uint8_t c) {
  if (c < f->first || c > f->last) return nullptr;
  return &f->glyph[c - f->first];
}

}  // namespace

size_t toGlyphs(const char *s, char *out, size_t n) {
  size_t o = 0;
  if (!n) return 0;
  const uint8_t *p = (const uint8_t *)(s ? s : "");
  while (*p && o + 1 < n) {
    uint32_t cp;
    int len;
    if (*p < 0x80) { cp = *p; len = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else { cp = *p & 0x07; len = 4; }
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) { len = i; cp = '?'; break; }
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    p += len;
    char c = '?';
    if (cp >= 0x20 && cp < 0x7F) c = (char)cp;
    else if (cp == 0x2026) {  // … → ..
      if (o + 2 < n) out[o++] = '.';
      c = '.';
    } else {
      for (const Extra &e : kExtras)
        if (e.cp == cp) { c = e.code; break; }
    }
    out[o++] = c;
  }
  out[o] = 0;
  return o;
}

int ascent(const GFXfont *f) {
  const GFXglyph *g = glyphFor(f, '8');
  if (!g) return f->yAdvance * 3 / 4;
  return -(int8_t)pgm_read_byte(&g->yOffset);
}

int widthGlyphs(const GFXfont *f, const char *s) {
  int w = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    const GFXglyph *g = glyphFor(f, *p);
    if (g) w += pgm_read_byte(&g->xAdvance);
    else {
      const GFXglyph *sp = glyphFor(f, ' ');
      w += sp ? pgm_read_byte(&sp->xAdvance) : 8;
    }
  }
  return w;
}

int textWidth(const GFXfont *f, const char *utf8) {
  char buf[128];
  toGlyphs(utf8, buf, sizeof buf);
  return widthGlyphs(f, buf);
}

static int drawGlyphs(Adafruit_GFX &d, const GFXfont *f, int x, int y, const char *buf, uint16_t color, Align a) {
  int w = widthGlyphs(f, buf);
  if (a == Center) x -= w / 2;
  else if (a == Right) x -= w;
  d.setFont(f);
  d.setTextWrap(false);
  d.setTextColor(color);
  d.setCursor(x, y);
  // Numeric fonts only carry ' '..'9'; anything else would be skipped silently,
  // so the caller picks a full font for text. Print char by char to bypass
  // Print's UTF-8-unaware paths.
  for (const char *p = buf; *p; p++) d.write((uint8_t)*p);
  return w;
}

int drawText(Adafruit_GFX &d, const GFXfont *f, int x, int y, const char *utf8, uint16_t color, Align a) {
  char buf[128];
  toGlyphs(utf8, buf, sizeof buf);
  return drawGlyphs(d, f, x, y, buf, color, a);
}

int drawTextFit(Adafruit_GFX &d, const GFXfont *f, int x, int y, const char *utf8, uint16_t color, int maxW,
                Align a) {
  char buf[128];
  size_t n = toGlyphs(utf8, buf, sizeof buf);
  if (widthGlyphs(f, buf) > maxW) {
    int dots = widthGlyphs(f, "..");
    while (n > 0 && widthGlyphs(f, buf) + dots > maxW) buf[--n] = 0;
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    if (n + 2 < sizeof buf) { buf[n++] = '.'; buf[n++] = '.'; buf[n] = 0; }
  }
  return drawGlyphs(d, f, x, y, buf, color, a);
}

}  // namespace panel

namespace panel {

// ── BigFont ─────────────────────────────────────────────────────────────

static const BigGlyph *bigGlyph(const BigFont *f, uint8_t c) {
  if (c < f->first || c > f->last) return nullptr;
  return &f->glyph[c - f->first];
}

static int bigWidth(const BigFont *f, const char *glyphs) {
  int w = 0;
  for (const uint8_t *p = (const uint8_t *)glyphs; *p; p++) {
    const BigGlyph *g = bigGlyph(f, *p);
    w += g ? g->adv : f->yAdvance / 4;
  }
  return w;
}

// Bits run on across rows (GFX packing); draw each row's runs as lines.
static void bigChar(Adafruit_GFX &d, const BigFont *f, const BigGlyph *g, int x, int y, uint16_t color) {
  const uint8_t *bm = f->bitmap + g->off;
  uint32_t bit = 0;
  for (int r = 0; r < g->h; r++) {
    int run = -1;
    for (int c = 0; c < g->w; c++, bit++) {
      bool on = (pgm_read_byte(&bm[bit >> 3]) >> (7 - (bit & 7))) & 1;
      if (on && run < 0) run = c;
      if (!on && run >= 0) {
        d.drawFastHLine(x + g->xo + run, y + g->yo + r, c - run, color);
        run = -1;
      }
    }
    if (run >= 0) d.drawFastHLine(x + g->xo + run, y + g->yo + r, g->w - run, color);
  }
}

int ascent(const VFont &f) {
  if (f.b) return f.b->ascent;
  return f.g ? ascent(f.g) : 0;
}

int textWidth(const VFont &f, const char *utf8) {
  if (!f.b) return f.g ? textWidth(f.g, utf8) : 0;
  char buf[64];
  toGlyphs(utf8, buf, sizeof buf);
  return bigWidth(f.b, buf);
}

int drawText(Adafruit_GFX &d, const VFont &f, int x, int y, const char *utf8, uint16_t color, Align a) {
  if (!f.b) return f.g ? drawText(d, f.g, x, y, utf8, color, a) : 0;
  char buf[64];
  toGlyphs(utf8, buf, sizeof buf);
  int w = bigWidth(f.b, buf);
  if (a == Center) x -= w / 2;
  else if (a == Right) x -= w;
  for (const uint8_t *p = (const uint8_t *)buf; *p; p++) {
    const BigGlyph *g = bigGlyph(f.b, *p);
    if (!g) {
      x += f.b->yAdvance / 4;
      continue;
    }
    if (g->w && g->h) bigChar(d, f.b, g, x, y, color);
    x += g->adv;
  }
  return w;
}

}  // namespace panel
