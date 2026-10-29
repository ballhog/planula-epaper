// An 800x480 drawing surface backed by one horizontal band of rows.
//
// The renderer draws the whole screen in screen coordinates; only pixels inside
// the current band [y0, y0 + rows) land in the buffer. Drawing the same scene
// once per band gives the full picture with a quarter of the RAM: 12 kB for
// 120-row bands instead of 48 kB for a whole 1-bit frame. On the panel those
// 36 kB are what TLS, Wi-Fi and the web page need while Hydros streams.
//
// Buffer layout matches GFXcanvas1: 1 bit per pixel, MSB first, 1 = ink.
#pragma once
#include <Adafruit_GFX.h>

namespace panel {

class BandCanvas : public Adafruit_GFX {
 public:
  BandCanvas(int16_t w, int16_t h, int16_t rows);
  ~BandCanvas();
  bool ok() const { return buf_ != nullptr; }

  void setBand(int16_t y0);  // also clears the band to paper
  int16_t bandY() const { return y0_; }
  int16_t bandRows() const { return rows_; }
  int bands() const { return (HEIGHT + rows_ - 1) / rows_; }
  uint8_t *buffer() { return buf_; }
  size_t bandBytes() const { return (size_t)((WIDTH + 7) / 8) * rows_; }

  void drawPixel(int16_t x, int16_t y, uint16_t c) override;
  void fillScreen(uint16_t c) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override;

 private:
  uint8_t *buf_;
  int16_t rows_, y0_ = 0;
  uint16_t stride_;
};

}  // namespace panel
