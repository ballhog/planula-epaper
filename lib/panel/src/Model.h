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
enum class Layout : uint8_t { Auto, Single, Grid2x2, Grid3x2, List, Timeline };
// How tiles split their space (the panel's own choice, a button on the reTerminal):
//   Both    - big number with the 24 h trend under it (the default)
//   Numbers - numbers only, as big as they fit
//   Graphs  - the trend fills the tile, with its 24 h low/high; the number is smaller
enum class View : uint8_t { Both, Numbers, Graphs };
const char *viewName(View v);
View viewFromName(const char *s);

// Timeline pages: each gauge is one lane, time runs left to right.
//   Level  - a value that ramps between points (light intensity, pump speed %)
//   Step   - a value that holds until the next point (setpoints, modes)
//   OnOff  - on while the value is > 0.5 (outlets, ATO, feed mode)
//   Events - one mark per point, value = amount (doser shots, feedings)
// Points left of "now" (t <= 0) happened; points right of it are scheduled.
enum class Lane : uint8_t { Level, Step, OnOff, Events };

const char *statusName(Status s);
Status statusFromName(const char *s);
const char *layoutName(Layout l);
Layout layoutFromName(const char *s);
const char *laneName(Lane k);
Lane laneFromName(const char *s);

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
  // y-range to draw the sparkline on (Coeno's spark_lo/spark_hi, already floored
  // to the input's minimum span so normal noise draws flat). Data outside widens it.
  bool hasSparkRange = false;
  float sparkLo = 0, sparkHi = 0;
  Status status = Status::Missing;
  uint32_t age_s = 0;    // age of the value when the page was built (kNoAge: don't show)
  uint8_t nspark = 0;    // sparkline points, oldest first
  float spark[kSparkLen];
  // Timeline lanes only: spark[i] is the value at t[i] (minutes from the page's
  // "now", negative = past), oldest first.
  Lane lane = Lane::Level;
  int16_t t[kSparkLen];
  // Battery modes: moved since the screen was last drawn (a small dot by the label).
  bool moved = false;
};

// Where the value is heading: +1 up, -1 down, 0 steady or not enough trend.
// Compares the value with ~3 h ago on the sparkline; "steady" unless it moved
// by a tenth of the normal band (or, without one, 1 % / five steps of the
// last shown decimal).
int trendOf(const Gauge &g);

struct Page {
  char title[28] = "";
  Layout layout = Layout::Auto;
  uint8_t n = 0;
  Gauge g[kMaxGauges];
  // Timeline pages: the window in minutes around "now", and the local time of
  // day at "now" (minutes after midnight, -1 = unknown: axis shows "-6 h").
  int16_t from = -720, to = 720;
  int16_t clock = -1;
  bool day = false;  // window is today, midnight to midnight
  bool h12 = true;   // 12-hour clock labels
  View view = View::Both;  // set by the panel, not by Coeno
  void clear() {
    title[0] = 0; layout = Layout::Auto; n = 0;
    from = -720; to = 720; clock = -1; day = false;
  }
};

// Timeline pages: move "now" on by `minutes` (the page got older on the panel).
void shiftTimeline(Page &p, int minutes);
// Sets from/to to midnight..midnight when p.day and the clock is known.
void dayWindow(Page &p);
// Value of a Level/Step/OnOff lane at time t (NAN outside its points).
float laneValueAt(const Gauge &g, float t);

// Everything around the gauges: header, footer, alarm banner.
constexpr uint32_t kNoAge = 0xFFFFFFFFu;  // battery snapshot: the footer gives the time

struct Chrome {
  char clock[8] = "";        // "08:41", empty if time unknown
  uint8_t page = 0, pages = 1;
  char footerLeft[72] = "";  // "Coeno · updated 40 s ago"
  char footerRight[48] = ""; // "Wi-Fi -58 · b12"
  bool footerAlert = false;  // stale/offline: footer drawn inverted
  int8_t wifi = -1;          // icon: -1 none, 0 offline (struck through), 1-4 bars
  int8_t battery = -1;       // icon + "96%": -1 none
  char room[24] = "";        // "72°F  45%" left of the icons, "" = none
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
