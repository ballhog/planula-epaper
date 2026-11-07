#include "Outlets.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Settings.h"

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

// History of the picked outputs: each entry is millis() of a change, with the
// new state in bit 0 (the time is rounded to an even ms for that).
struct Hist {
  char key[44];
  uint8_t len;
  uint32_t ev[kHist];
};
Hist hist[kPicked];

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

void record(Hist &h, bool on) {
  uint32_t e = (millis() & ~1u) | (on ? 1u : 0u);
  if (h.len && (h.ev[h.len - 1] & 1u) == (on ? 1u : 0u)) return;  // no change
  if (h.len >= kHist) {
    memmove(h.ev, h.ev + 1, sizeof(uint32_t) * (kHist - 1));
    h.len = kHist - 1;
  }
  h.ev[h.len++] = e;
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

void update(const char *key, const char *source, const char *type, bool on, bool manual, bool hidden) {
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
  o->atMs = millis();
  if (picked(key))
    if (Hist *h = histFor(key, true)) record(*h, on);
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
  uint32_t ms = millis();
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
    g.has = true;
    g.value = o->on ? 1 : 0;
    if (o->manual) PANEL_COPY(g.note, "by hand");
    PANEL_COPY(g.src, o->source);
    Hist *h = histFor(o->key, false);
    if (!h) continue;
    // Points: the state at the window start (the last change before it), then
    // each change inside it.
    int first = h->len;
    for (int k = 0; k < h->len; k++)
      if ((int32_t)(ms - (h->ev[k] & ~1u)) / 60000 <= -p.from) {
        first = k;
        break;
      }
    int start = first > 0 ? first - 1 : 0;
    for (int k = start; k < h->len && g.nspark < panel::kSparkLen; k++) {
      int32_t ago = (int32_t)(ms - (h->ev[k] & ~1u)) / 60000;
      g.t[g.nspark] = (int16_t)(ago > -p.from ? p.from : -ago);
      g.spark[g.nspark] = (h->ev[k] & 1u) ? 1 : 0;
      g.nspark++;
    }
  }
  return true;
}

}  // namespace outlets
