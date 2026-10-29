#include "Display.h"

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <epd/GxEPD2_750_T7.h>
#include <gdey/GxEPD2_750_GDEY075T7.h>

#include "Render.h"
#include "Settings.h"

namespace display {

namespace {

GFXcanvas1 *cv = nullptr;
GxEPD2_EPD *epd = nullptr;
uint32_t lastHash = 0, partials = 0, nRefresh = 0, nFull = 0, lastFullMs = 0;
bool first = true, fullAsked = false;

uint32_t hashFrame() {
  // FNV-1a over the frame
  const uint8_t *b = cv->getBuffer();
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < (size_t)panel::kW / 8 * panel::kH; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

void push(bool full) {
  // Canvas: 1 = ink. GxEPD2 bitmaps: 1 = white, hence invert = true.
  const uint8_t *buf = cv->getBuffer();
  if (full) {
    epd->writeImageForFullRefresh(buf, 0, 0, panel::kW, panel::kH, true, false, false);
    epd->refresh(false);
    epd->writeImageAgain(buf, 0, 0, panel::kW, panel::kH, true, false, false);
    nFull++;
    lastFullMs = millis();
    partials = 0;
  } else {
    epd->writeImage(buf, 0, 0, panel::kW, panel::kH, true, false, false);
    epd->refresh(true);
    epd->writeImageAgain(buf, 0, 0, panel::kW, panel::kH, true, false, false);
    partials++;
  }
  epd->powerOff();  // panel voltages off between updates: no fading, less wear
  nRefresh++;
}

}  // namespace

void begin(const String &driver) {
  cv = new GFXcanvas1(panel::kW, panel::kH);
  // CS, DC and RST are plain GPIOs that GxEPD2 drives itself. Don't hand CS to
  // the SPI peripheral (SPI.begin's ss argument): the core then refuses
  // digitalWrite on it and the panel is never selected (build 1 bug).
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);
  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  SPI.end();
  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
  if (driver == "gdey") epd = new GxEPD2_750_GDEY075T7(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);
  else epd = new GxEPD2_750_T7(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);
  // Waveshare boards want a 2 ms reset pulse.
  epd->init(115200, true, 2, false);
}

GFXcanvas1 &canvas() { return *cv; }

bool show(bool forceFull) {
  uint32_t h = hashFrame();
  bool changed = h != lastHash || first;
  const Settings &s = settings::get();
  bool full = forceFull || first || fullAsked || partials >= s.fullEvery || millis() - lastFullMs > 3600000UL;
  if (!changed && !full) return false;
  if (!changed && full && !forceFull && !fullAsked && !first) {
    // Nothing new: the hourly clean-up can wait for the next real change,
    // unless it's overdue by a lot (ghosting builds slowly on a static page).
    if (millis() - lastFullMs < 4UL * 3600000UL) return false;
  }
  push(full);
  first = false;
  fullAsked = false;
  lastHash = h;
  return true;
}

void flash(int times) {
  uint8_t *b = cv->getBuffer();
  size_t n = (size_t)panel::kW / 8 * panel::kH;
  for (int i = 0; i < times * 2; i++) {
    for (size_t k = 0; k < n; k++) b[k] ^= 0xFF;
    push(false);
  }
  lastHash = hashFrame();
}

void requestFull() { fullAsked = true; }
uint32_t refreshes() { return nRefresh; }
uint32_t fullRefreshes() { return nFull; }

size_t bmpSize() { return 62 + (size_t)panel::kW / 8 * panel::kH; }

void bmpHeader(uint8_t *o) {
  const uint32_t rowBytes = panel::kW / 8;  // 100, already a multiple of 4
  const uint32_t img = rowBytes * panel::kH;
  const uint32_t size = 62 + img;
  memset(o, 0, 62);
  o[0] = 'B'; o[1] = 'M';
  memcpy(o + 2, &size, 4);
  uint32_t off = 62; memcpy(o + 10, &off, 4);
  uint32_t hdr = 40; memcpy(o + 14, &hdr, 4);
  int32_t w = panel::kW, hgt = panel::kH; memcpy(o + 18, &w, 4); memcpy(o + 22, &hgt, 4);
  uint16_t planes = 1, bpp = 1; memcpy(o + 26, &planes, 2); memcpy(o + 28, &bpp, 2);
  memcpy(o + 34, &img, 4);
  uint32_t colors = 2; memcpy(o + 46, &colors, 4);
  // palette: index 0 = paper, 1 = ink
  o[54] = 0xEC; o[55] = 0xEC; o[56] = 0xEC;
  o[58] = 0x1C; o[59] = 0x1C; o[60] = 0x1C;
}

const uint8_t *row(int y) { return cv->getBuffer() + (size_t)y * (panel::kW / 8); }

}  // namespace display
