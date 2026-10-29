// What the panel shows, independent of where it came from (Coeno, Apex, Hydros).
// Pure data: no Arduino, no network. Rendered by Render.cpp; filled by
// PageJson.cpp (Coeno pages) or the panel's own page builder (direct mode).
#pragma once
#include <stdint.h>
#include <string.h>

namespace panel {

constexpr int kMaxGauges = 10;
constexpr int kMaxPages = 6;
constexpr int kSparkLen = 48;

enum class Status : uint8_t { Ok, Warn, Alarm, Stale, Missing };
enum class Layout : uint8_t { Auto, Single, Grid2x2, Grid3x2, List };

const char *statusName(Status s);
Status statusFromName(const char *s);
const char *layoutName(Layout l);
Layout layoutFromName(const char *s);

struct Gauge {
  char id[40] = "";      // stable key ("tank:ph", "apex:Tmp", ...)
  char label[28] = "";   // "Tank pH"
  char unit[12] = "";    // "°C", "ppt", "" for pH
  char src[16] = "";     // "Apex", "Hydros", "Calyx" (small tag)
  char note[16] = "";    // short status word: "LOW", "HIGH", set by whoever sets status
  bool has = false;      // value present
  float value = 0;
  uint8_t dec = 1;       // decimals shown
  bool hasRange = false; // normal band, drawn on the sparkline
  float lo = 0, hi = 0;
  Status status = Status::Missing;
  uint32_t age_s = 0;    // age of the value when the page was built
  uint8_t nspark = 0;    // sparkline points, oldest first
  float spark[kSparkLen];
};

struct Page {
  char title[28] = "";
  Layout layout = Layout::Auto;
  uint8_t n = 0;
  Gauge g[kMaxGauges];
  void clear() { title[0] = 0; layout = Layout::Auto; n = 0; }
};

// Everything around the gauges: header, footer, alarm banner.
struct Chrome {
  char clock[8] = "";        // "08:41", empty if time unknown
  uint8_t page = 0, pages = 1;
  char footerLeft[72] = "";  // "Coeno · updated 40 s ago"
  char footerRight[48] = ""; // "Wi-Fi -58 · b12"
  bool footerAlert = false;  // stale/offline: footer drawn inverted
  char alarm[56] = "";       // banner text, "" = none
  uint8_t alarmCount = 0;
  bool alarmAcked = false;   // acknowledged: banner shown hollow instead of solid
  bool invert = false;       // whole screen inverted (alarm flash frames)
};

// Age as a short word: "now", "40 s", "7 min", "3 h", "2 d".
void formatAge(uint32_t s, char *out, size_t n);
// Value as text with the gauge's decimals ("8.12", "--" when missing).
void formatValue(const Gauge &g, char *out, size_t n);

// Copy with truncation, always terminated.
inline void copyStr(char *dst, size_t n, const char *src) {
  if (!n) return;
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}
#define PANEL_COPY(dst, src) ::panel::copyStr((dst), sizeof(dst), (src))

}  // namespace panel
