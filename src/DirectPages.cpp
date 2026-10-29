#include "DirectPages.h"

#include "PageJson.h"
#include "Readings.h"
#include "Settings.h"

namespace direct {

int pageCount() {
  const Settings &s = settings::get();
  int n = (int)s.pages.size();
  for (auto &g : s.gauges) n = max(n, g.page + 1);
  return constrain(n, 1, panel::kMaxPages);
}

static float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }

void build(int i, panel::Page &p) {
  const Settings &s = settings::get();
  p.clear();
  if (i < (int)s.pages.size()) {
    PANEL_COPY(p.title, s.pages[i].title.c_str());
    p.layout = panel::layoutFromName(s.pages[i].layout.c_str());
  } else {
    snprintf(p.title, sizeof p.title, "Page %d", i + 1);
  }
  for (auto &gc : s.gauges) {
    if (gc.page != i || p.n >= panel::kMaxGauges) continue;
    panel::Gauge &g = p.g[p.n++];
    g = panel::Gauge();
    PANEL_COPY(g.id, gc.key.c_str());
    g.dec = gc.dec;
    readings::Reading r;
    bool have = readings::get(gc.key, r);
    String label = gc.label.length() ? gc.label : (have ? String(r.name) : gc.key);
    PANEL_COPY(g.label, label.c_str());
    String unit = gc.unit;
    bool toF = have && !strcmp(r.kind, "temp") && s.tempF;
    if (!unit.length() && have) unit = toF ? String("°F") : String(r.unit);
    PANEL_COPY(g.unit, unit.c_str());
    if (have) {
      PANEL_COPY(g.src, r.source);
      g.has = !isnan(r.value);
      g.value = toF ? cToF(r.value) : r.value;
      g.age_s = (millis() - r.atMs) / 1000;
      g.nspark = r.nspark;
      for (int k = 0; k < r.nspark; k++) {
        float v = readings::sparkAt(r, k);
        g.spark[k] = toF ? cToF(v) : v;
      }
    }
    panel::Limits L{gc.warnLo, gc.warnHi, gc.alarmLo, gc.alarmHi};
    panel::evaluate(g, L, s.stale_s);
  }
}

}  // namespace direct
