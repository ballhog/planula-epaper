#include "Model.h"

#include <math.h>
#include <stdio.h>

namespace panel {

const char *statusName(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Warn: return "warn";
    case Status::Alarm: return "alarm";
    case Status::Stale: return "stale";
    default: return "missing";
  }
}

Status statusFromName(const char *s) {
  if (!s) return Status::Missing;
  if (!strcmp(s, "ok")) return Status::Ok;
  if (!strcmp(s, "warn")) return Status::Warn;
  if (!strcmp(s, "alarm") || !strcmp(s, "bad")) return Status::Alarm;
  if (!strcmp(s, "stale")) return Status::Stale;
  return Status::Missing;
}

const char *layoutName(Layout l) {
  switch (l) {
    case Layout::Single: return "single";
    case Layout::Grid2x2: return "2x2";
    case Layout::Grid3x2: return "3x2";
    case Layout::List: return "list";
    case Layout::Timeline: return "timeline";
    default: return "auto";
  }
}

Layout layoutFromName(const char *s) {
  if (!s) return Layout::Auto;
  if (!strcmp(s, "single") || !strcmp(s, "big")) return Layout::Single;
  if (!strcmp(s, "2x2")) return Layout::Grid2x2;
  if (!strcmp(s, "3x2")) return Layout::Grid3x2;
  if (!strcmp(s, "list")) return Layout::List;
  if (!strcmp(s, "timeline") || !strcmp(s, "schedule")) return Layout::Timeline;
  return Layout::Auto;
}

const char *laneName(Lane k) {
  switch (k) {
    case Lane::Step: return "step";
    case Lane::OnOff: return "onoff";
    case Lane::Events: return "events";
    default: return "level";
  }
}

Lane laneFromName(const char *s) {
  if (!s) return Lane::Level;
  if (!strcmp(s, "step")) return Lane::Step;
  if (!strcmp(s, "onoff") || !strcmp(s, "outlet") || !strcmp(s, "switch")) return Lane::OnOff;
  if (!strcmp(s, "events") || !strcmp(s, "dose") || !strcmp(s, "doses")) return Lane::Events;
  return Lane::Level;
}

void dayWindow(Page &p) {
  if (!p.day || p.clock < 0) return;
  p.from = -p.clock;
  p.to = 1440 - p.clock;
}

void shiftTimeline(Page &p, int minutes) {
  if (minutes <= 0 || p.layout != Layout::Timeline) return;
  for (int i = 0; i < p.n; i++) {
    Gauge &g = p.g[i];
    for (int k = 0; k < g.nspark; k++) g.t[k] = (int16_t)(g.t[k] - minutes);
  }
  if (p.clock >= 0) p.clock = (int16_t)((p.clock + minutes) % 1440);
  dayWindow(p);
}

float laneValueAt(const Gauge &g, float t) {
  int n = g.nspark;
  if (n == 0 || t < g.t[0]) return NAN;
  if (t >= g.t[n - 1]) return g.spark[n - 1];  // holds after the last point
  for (int i = 0; i + 1 < n; i++) {
    if (t < g.t[i + 1]) {
      if (g.lane != Lane::Level || g.t[i + 1] == g.t[i]) return g.spark[i];
      float f = (t - g.t[i]) / (float)(g.t[i + 1] - g.t[i]);
      return g.spark[i] + f * (g.spark[i + 1] - g.spark[i]);
    }
  }
  return g.spark[n - 1];
}

void formatAge(uint32_t s, char *out, size_t n) {
  if (s < 10) snprintf(out, n, "now");
  else if (s < 90) snprintf(out, n, "%u s", (unsigned)s);
  else if (s < 90 * 60) snprintf(out, n, "%u min", (unsigned)((s + 30) / 60));
  else if (s < 36 * 3600) snprintf(out, n, "%u h", (unsigned)((s + 1800) / 3600));
  else snprintf(out, n, "%u d", (unsigned)((s + 43200) / 86400));
}

void formatValue(const Gauge &g, char *out, size_t n) {
  if (!g.has || isnan(g.value)) {
    snprintf(out, n, "--");
    return;
  }
  int d = g.dec > 4 ? 4 : g.dec;
  snprintf(out, n, "%.*f", d, (double)g.value);
  if (!strcmp(out, "-0") || !strcmp(out, "-0.0") || !strcmp(out, "-0.00")) memmove(out, out + 1, strlen(out));
}

}  // namespace panel
