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
    default: return "auto";
  }
}

Layout layoutFromName(const char *s) {
  if (!s) return Layout::Auto;
  if (!strcmp(s, "single") || !strcmp(s, "big")) return Layout::Single;
  if (!strcmp(s, "2x2")) return Layout::Grid2x2;
  if (!strcmp(s, "3x2")) return Layout::Grid3x2;
  if (!strcmp(s, "list")) return Layout::List;
  return Layout::Auto;
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
