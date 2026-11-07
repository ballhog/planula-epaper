// Alarms across all pages: collect, flash and beep on new ones.
// Split out of main.cpp; shared state in Shared.h.
#include "Shared.h"

namespace ui {

void collectAlarms() {
  // Building every page is only worth it when something they're built from
  // changed: readings, a Coeno page, settings, the page count.
  static uint32_t seenR = 0, seenC = 0, seenS = 0;
  static int seenN = -1;
  static uint32_t seenAt = 0;  // and at least once a minute: values go stale with time
  uint32_t r = readings::version(), cp = coeno::lastPageMs(), sg = settings::generation();
  if (r == seenR && cp == seenC && sg == seenS && nPages == seenN && millis() - seenAt < 60000) return;
  seenAt = millis();
  seenR = r;
  seenC = cp;
  seenS = sg;
  seenN = nPages;
  std::vector<Alarm> now;
  std::unique_ptr<panel::Page> pp(new (std::nothrow) panel::Page());  // ~4.4 kB: not on the loop stack
  if (!pp) return;
  panel::Page &p = *pp;
  for (int i = 0; i < nPages; i++) {
    if (!fetchPage(i, p)) continue;
    for (int k = 0; k < p.n; k++) {
      const panel::Gauge &g = p.g[k];
      if (g.status != panel::Status::Alarm) continue;
      bool dup = false;
      for (auto &a : now) dup |= a.id == g.id;
      if (dup) continue;
      char val[16];
      panel::formatValue(g, val, sizeof val);
      String label = g.label;
      label.toUpperCase();
      String t = label + (g.note[0] ? String(" ") + g.note : "") + " " + val + (g.unit[0] ? String(" ") + g.unit : "");
      now.push_back({g.id, t});
    }
  }
  // A new alarm id → flash and un-acknowledge.
  bool fresh = false;
  for (auto &a : now) {
    bool seen = false;
    for (auto &k : knownAlarmIds) seen |= k == a.id;
    if (!seen) fresh = true;
  }
  knownAlarmIds.clear();
  for (auto &a : now) knownAlarmIds.push_back(a.id);
  if (fresh) {
    acked = false;
    flashAsked = true;
    lastReminder = millis();
    board::alarmBeep();
  }
  if (now.empty()) acked = false;
  alarms = now;
}

}  // namespace ui
