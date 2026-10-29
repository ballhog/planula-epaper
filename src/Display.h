// The e-paper: Waveshare 7.5" B/W V2 (800x480) on the Waveshare ESP32 driver
// board Rev 3. One 48 kB frame (GFXcanvas1) is drawn by lib/panel and pushed
// with GxEPD2's driver class directly.
//
// Refresh rules:
//   - unchanged frame → nothing (no wear, no flicker)
//   - changed → partial refresh (< 1 s, no flash)
//   - full flashing refresh every `fullEvery` partials, at least hourly, and on
//     the first frame, to clear ghosting
//   - flash(): alarm attention, the whole screen inverts a few times
#pragma once
#include <Adafruit_GFX.h>

namespace display {

// Board pins (Waveshare e-Paper ESP32 Driver Board, same on Rev 2/3).
constexpr int PIN_SCK = 13, PIN_MOSI = 14, PIN_CS = 15, PIN_RST = 26, PIN_DC = 27, PIN_BUSY = 25;

void begin(const String &driver);  // "t7" or "gdey"
GFXcanvas1 &canvas();
// Push the canvas. force_full = flashing refresh. Returns true if anything changed.
bool show(bool forceFull = false);
void flash(int times);
uint32_t refreshes();
uint32_t fullRefreshes();
// The current frame as a 1-bit BMP, for the settings page preview.
size_t bmpSize();
void writeBmp(uint8_t *out);
// Ask for a full refresh with the next show().
void requestFull();

}  // namespace display
