#include "BandCanvas.h"

#include <stdlib.h>
#include <string.h>

namespace panel {

BandCanvas::BandCanvas(int16_t w, int16_t h, int16_t rows) : Adafruit_GFX(w, h), rows_(rows) {
  stride_ = (w + 7) / 8;
  buf_ = (uint8_t *)malloc((size_t)stride_ * rows_);
  if (buf_) memset(buf_, 0, (size_t)stride_ * rows_);
}

BandCanvas::~BandCanvas() { free(buf_); }

void BandCanvas::setBand(int16_t y0) {
  y0_ = y0;
  if (buf_) memset(buf_, 0, (size_t)stride_ * rows_);
}

// No rotation support (the panel is used at rotation 0).
void BandCanvas::drawPixel(int16_t x, int16_t y, uint16_t c) {
  if (!buf_ || x < 0 || x >= WIDTH) return;
  int16_t r = y - y0_;
  if (r < 0 || r >= rows_) return;
  uint8_t *p = buf_ + (size_t)r * stride_ + (x >> 3);
  uint8_t m = 0x80 >> (x & 7);
  if (c) *p |= m;
  else *p &= ~m;
}

void BandCanvas::fillScreen(uint16_t c) {
  if (buf_) memset(buf_, c ? 0xFF : 0x00, (size_t)stride_ * rows_);
}

void BandCanvas::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) {
  int16_t r = y - y0_;
  if (!buf_ || r < 0 || r >= rows_ || w <= 0) return;
  int16_t x0 = x < 0 ? 0 : x, x1 = x + w > WIDTH ? WIDTH : x + w;  // [x0, x1)
  if (x0 >= x1) return;
  uint8_t *row = buf_ + (size_t)r * stride_;
  for (int16_t i = x0; i < x1;) {
    if ((i & 7) == 0 && i + 8 <= x1) {  // whole bytes
      int16_t n = (x1 - i) >> 3;
      memset(row + (i >> 3), c ? 0xFF : 0x00, n);
      i += n * 8;
      continue;
    }
    uint8_t m = 0x80 >> (i & 7);
    if (c) row[i >> 3] |= m;
    else row[i >> 3] &= ~m;
    i++;
  }
}

void BandCanvas::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) {
  if (!buf_ || x < 0 || x >= WIDTH || h <= 0) return;
  int16_t a = y - y0_, b = a + h;  // [a, b) in band rows
  if (a < 0) a = 0;
  if (b > rows_) b = rows_;
  uint8_t m = 0x80 >> (x & 7);
  for (int16_t r = a; r < b; r++) {
    uint8_t *p = buf_ + (size_t)r * stride_ + (x >> 3);
    if (c) *p |= m;
    else *p &= ~m;
  }
}

void BandCanvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  int16_t a = y < y0_ ? y0_ : y, b = y + h > y0_ + rows_ ? y0_ + rows_ : y + h;
  for (int16_t yy = a; yy < b; yy++) drawFastHLine(x, yy, w, c);
}

}  // namespace panel
