#include "Outlets.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Rules.h"
#include "Settings.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

#include <type_traits>

namespace outlets {

namespace {

SemaphoreHandle_t mu = nullptr;
struct Lock {
  Lock() {
    if (!mu) mu = xSemaphoreCreateMutex();
    xSemaphoreTake(mu, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(mu); }
};

Out items[kMax];
int n = 0;

// History of the picked outputs (rules::OutletHistory: level changes, small
// ramp steps dropped).
struct Hist {
  char key[44];
  rules::OutletHistory h;
};
// In RTC memory: battery modes deep-sleep between readings, and ordinary RAM
// (and millis()) start over on every wake, which left the Outlets page empty
// after a night on battery. Loaded fresh on power-on / flashing.
RTC_DATA_ATTR Hist hist[kPicked];
static_assert(std::is_trivially_default_constructible<Hist>::value, "Hist must be plain data (RTC memory)");

// Wall clock in ms, wrapping at 2^32 (rules::since() handles the wrap). Keeps
// counting through deep sleep, unlike millis(). 0 = clock not set yet.
uint32_t clockMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint32_t)((uint64_t)tv.tv_sec * 1000u + tv.tv_usec / 1000);
}

bool picked(const char *key) {
  for (auto &o : settings::get().outlets)
    if (o.key == key) return true;
  return false;
}

Hist *histFor(const char *key, bool create) {
  for (auto &h : hist)
    if (h.key[0] && !strcmp(h.key, key)) return &h;
  if (!create) return nullptr;
  // Reuse a slot whose output is no longer picked.
  for (auto &h : hist)
    if (!h.key[0] || !picked(h.key)) {
      memset(&h, 0, sizeof h);
      strlcpy(h.key, key, sizeof h.key);
      return &h;
    }
  return nullptr;
}

}  // namespace

// "apex:Return" -> "Return"; "hydros:<controller>/1RTunzeSmp" -> "1RTunzeSmp".
const char *nameOf(const char *key) {
  const char *c = strchr(key, ':');
  c = c ? c + 1 : key;
  if (!strncmp(key, "hydros:", 7)) {
    const char *sl = strchr(c, '/');
    if (sl) c = sl + 1;
  }
  return c;
}

void update(const char *key, const char *source, const char *type, uint8_t level, bool manual, bool hidden) {
  bool on = level > 0;
  Lock l;
  Out *o = nullptr;
  for (int i = 0; i < n; i++)
    if (!strcmp(items[i].key, key)) o = &items[i];
  if (!o) {
    if (n >= kMax) return;
    o = &items[n++];
    memset(o, 0, sizeof *o);
    strlcpy(o->key, key, sizeof o->key);
  }
  strlcpy(o->source, source, sizeof o->source);
  strlcpy(o->type, type, sizeof o->type);
  o->on = on;
  o->manual = manual;
  o->hidden = hidden;
  o->level = level > 100 ? 100 : level;
  if (level > 0 && level < 100) o->variable = true;
  o->atMs = millis();
  uint32_t now = clockMs();
  if (now && picked(key))  // no history until the clock is set (seconds after boot)
    if (Hist *h = histFor(key, true)) h->h.record(now, o->level);
}

int count() {
  Lock l;
  return n;
}

bool at(int i, Out &out) {
  Lock l;
  if (i < 0 || i >= n) return false;
  out = items[i];
  return true;
}

bool get(const String &key, Out &out) {
  Lock l;
  for (int i = 0; i < n; i++)
    if (key == items[i].key) {
      out = items[i];
      return true;
    }
  return false;
}

bool buildPage(panel::Page &p) {
  const Settings &s = settings::get();
  p.clear();
  PANEL_COPY(p.title, "Outlets");
  p.layout = panel::Layout::Timeline;
  p.from = -720;  // the last 12 h, up to now
  p.to = 0;
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    p.clock = tm.tm_hour * 60 + tm.tm_min;
  }
  Lock l;
  uint32_t ms = clockMs();
  for (auto &c : s.outlets) {
    if (p.n >= panel::kMaxGauges) break;
    panel::Gauge &g = p.g[p.n++];
    PANEL_COPY(g.id, c.key.c_str());
    PANEL_COPY(g.label, c.label.length() ? c.label.c_str() : nameOf(c.key.c_str()));
    g.lane = panel::Lane::OnOff;
    g.status = panel::Status::Ok;
    Out *o = nullptr;
    for (int i = 0; i < n; i++)
      if (c.key == items[i].key) o = &items[i];
    if (!o) {
      g.has = false;
      g.status = panel::Status::Missing;
      continue;
    }
    bool doser = !strcmp(o->type, "doser");
    g.lane = doser ? panel::Lane::Events : o->variable ? panel::Lane::Step : panel::Lane::OnOff;
    g.has = true;
    if (o->variable) {
      g.value = o->level;
      PANEL_COPY(g.unit, "%");
      g.dec = 0;
    } else {
      g.value = o->on ? 1 : 0;
    }
    if (o->manual) PANEL_COPY(g.note, "by hand");
    PANEL_COPY(g.src, o->source);
    Hist *h = ms ? histFor(o->key, false) : nullptr;
    if (!h) continue;
    g.nspark = (uint8_t)rules::lanePoints(h->h, ms, p.from, doser, o->variable, g.t, g.spark, panel::kSparkLen);
  }
  return true;
}

}  // namespace outlets
