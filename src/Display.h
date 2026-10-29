// The e-paper: Waveshare 7.5" B/W V2 (800x480) on the Waveshare ESP32 driver
// board Rev 3. A screen is a Scene: a function that draws the whole 800x480
// picture. It is drawn four times into one 12 kB band (panel::BandCanvas), each
// band pushed straight to the panel's own RAM, so the ESP32 never holds a whole
// 48 kB frame. The last scene is kept to redraw /screen.bmp rows and flashes.
//
// Refresh rules:
//   - unchanged frame → nothing (no wear, no flicker)
//   - changed → partial refresh (< 1 s, no flash)
//   - full flashing refresh every `fullEvery` partials, at least hourly, and on
//     the first frame, to clear ghosting
//   - flash(): alarm attention, the whole screen inverts a few times
#pragma once
#include <Adafruit_GFX.h>

#include <functional>

namespace display {

#if defined(BOARD_RETERMINAL)
// Seeed reTerminal E1001. The microSD card shares SCK/MOSI; its CS (14) is held high.
constexpr int PIN_SCK = 7, PIN_MOSI = 9, PIN_CS = 10, PIN_RST = 12, PIN_DC = 11, PIN_BUSY = 13;
constexpr int PIN_SD_CS = 14;
#else
// Board pins (Waveshare e-Paper ESP32 Driver Board, same on Rev 2/3).
constexpr int PIN_SCK = 13, PIN_MOSI = 14, PIN_CS = 15, PIN_RST = 26, PIN_DC = 27, PIN_BUSY = 25;
constexpr int PIN_SD_CS = -1;
#endif

void begin(const String &driver);  // "t7" or "gdey"
using Scene = std::function<void(Adafruit_GFX &)>;
// Draw a scene and push it. forceFull = flashing refresh. Returns true if
// anything changed. The scene must own what it draws (capture by value).
bool show(const Scene &scene, bool forceFull = false);
void flash(int times);
uint32_t refreshes();
uint32_t fullRefreshes();
// The current frame as a 1-bit BMP, for the settings page preview.
size_t bmpSize();
void bmpHeader(uint8_t out[62]);  // 1-bit BMP header + palette
const uint8_t *row(int y);        // one 100-byte row of the frame (1 = ink); redraws its band
// Ask for a full refresh with the next show().
void requestFull();

}  // namespace display
