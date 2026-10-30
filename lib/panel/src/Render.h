// Draws a Page (or a setup / message screen) onto any Adafruit_GFX surface.
// Colour 1 = black ink, 0 = white paper. The panel draws into a GFXcanvas1 and
// pushes it to the e-paper; tools/host draws the same code into a PNG.
#pragma once
#include <Adafruit_GFX.h>

#include "Model.h"

namespace panel {

constexpr int kW = 800, kH = 480;
constexpr uint16_t INK = 1, PAPER = 0;

// Full screen: header (or alarm banner), gauges, footer.
void render(Adafruit_GFX &d, const Page &p, const Chrome &c);

// First-boot / no-network screen: how to reach the settings page.
struct SetupInfo {
  const char *apName = "";   // Wi-Fi network the panel is hosting
  const char *apPass = "";   // its password
  const char *url = "";      // http://192.168.4.1
  const char *reason = "";   // "No Wi-Fi saved yet" / "Can't reach <ssid>"
};
void renderSetup(Adafruit_GFX &d, const SetupInfo &s, const Chrome &c);

// Plain message screen ("Connecting…", "Updating 40 %", "Waiting for Coeno").
// body may contain '\n'. big = optional large line (e.g. the IP address).
void renderMessage(Adafruit_GFX &d, const char *title, const char *body, const char *big, const Chrome &c);

// System screen: firmware update state on the left, panel details on the right.
struct SystemInfo {
  enum State : uint8_t { Unknown, Checking, UpToDate, Available, Installing, Failed };
  State state = Unknown;
  long build = 0;              // running build
  const char *version = "";    // "v1.2-3-gabc"
  long availBuild = 0;         // newer build, when Available
  const char *availFrom = "";  // "Coeno" / "GitHub"
  const char *detail = "";     // last check result / failure reason
  const char *checked = "";    // "checked 2:10 PM" / "checked 40 min ago"
  int pct = -1;                // Installing: download %, -1 = not known yet
  const char *hint = "";       // what the button does here
  const char *keys[8] = {};    // details, right column
  const char *vals[8] = {};
  uint8_t nrows = 0;
};
void renderSystem(Adafruit_GFX &d, const SystemInfo &s, const Chrome &c);

// Which grid a page ends up with (exposed for tests): cols x rows, or 0x0 = list.
void gridFor(const Page &p, int &cols, int &rows);

}  // namespace panel
