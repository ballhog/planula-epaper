#include "Display.h"

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <epd/GxEPD2_750_T7.h>
#include <gdey/GxEPD2_750_GDEY075T7.h>

#include "BandCanvas.h"
#include "Render.h"
#include "Settings.h"

namespace display {

namespace {

constexpr int16_t kBand = 120;  // 4 bands of 12 kB
panel::BandCanvas *cv = nullptr;
GxEPD2_EPD *epd = nullptr;
Scene last;
int16_t bandY = -1;  // band currently in cv, -1 = none / stale
uint32_t lastHash = 0, partials = 0, nRefresh = 0, nFull = 0, lastFullMs = 0;
bool first = true, fullAsked = false;

void drawBand(const Scene &scene, int16_t y0) {
  cv->setBand(y0);
  if (scene) scene(*cv);
  bandY = y0;
}

uint32_t hashScene(const Scene &scene) {
  // FNV-1a over the frame, band by band (same value a whole-frame hash gives)
  uint32_t h = 2166136261u;
  for (int16_t y0 = 0; y0 < panel::kH; y0 += kBand) {
    drawBand(scene, y0);
    const uint8_t *b = cv->buffer();
    for (size_t i = 0; i < cv->bandBytes(); i++) h = (h ^ b[i]) * 16777619u;
  }
  return h;
}

enum class Write { Full, Partial, Again };

void writeAll(Write how, bool inverted) {
  // Band: 1 = ink. GxEPD2 bitmaps: 1 = white, hence invert = true.
  for (int16_t y0 = 0; y0 < panel::kH; y0 += kBand) {
    drawBand(last, y0);
    const uint8_t *buf = cv->buffer();
    int16_t h = panel::kH - y0 < kBand ? panel::kH - y0 : kBand;
    bool inv = !inverted;
    if (how == Write::Full) epd->writeImageForFullRefresh(buf, 0, y0, panel::kW, h, inv, false, false);
    else if (how == Write::Partial) epd->writeImage(buf, 0, y0, panel::kW, h, inv, false, false);
    else epd->writeImageAgain(buf, 0, y0, panel::kW, h, inv, false, false);
  }
}

void push(bool full, bool inverted = false) {
  writeAll(full ? Write::Full : Write::Partial, inverted);
  epd->refresh(!full);
  writeAll(Write::Again, inverted);
  if (full) {
    nFull++;
    lastFullMs = millis();
    partials = 0;
  } else {
    partials++;
  }
  epd->powerOff();  // panel voltages off between updates: no fading, less wear
  nRefresh++;
}

}  // namespace

void begin(const String &driver) {
  cv = new panel::BandCanvas(panel::kW, panel::kH, kBand);
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

bool show(const Scene &scene, bool forceFull) {
  last = scene;
  uint32_t h = hashScene(last);
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
  for (int i = 0; i < times * 2; i++) push(false, i % 2 == 0);
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

const uint8_t *row(int y) {
  int16_t y0 = (int16_t)(y / kBand * kBand);
  if (bandY != y0) drawBand(last, y0);
  return cv->buffer() + (size_t)(y - y0) * (panel::kW / 8);
}

}  // namespace display
