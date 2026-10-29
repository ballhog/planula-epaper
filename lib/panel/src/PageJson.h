// Page wire format (Coeno → panel, retained MQTT panel_node/<panel>/page/<n>):
//
// {
//   "v": 1, "title": "Tank", "layout": "auto" | "single" | "2x2" | "3x2" | "list",
//   "gauges": [
//     { "id": "tank:ph", "label": "pH", "value": 8.12, "dec": 2, "unit": "",
//       "src": "Apex", "status": "ok" | "warn" | "alarm" | "stale" | "missing",
//       "note": "HIGH", "age_s": 30, "lo": 7.9, "hi": 8.4,
//       "spark": [8.10, 8.11, ...] }            // oldest first, up to 48
//   ]
// }
//
// value null/absent = no reading. lo/hi = the normal band (drawn on the sparkline).
// Coeno decides status; the panel only adds "stale" when the page itself goes old.
#pragma once
#include <stddef.h>

#include "Model.h"

namespace panel {

// Returns "" on success, or a short error.
const char *parsePage(const char *json, size_t len, Page &out);

// Limits a gauge is judged against in direct mode (Apex / Hydros straight to
// the panel, no Coeno). Unset bounds are NAN.
struct Limits {
  float warnLo, warnHi;    // normal band: outside = warn
  float alarmLo, alarmHi;  // outside = alarm
};
// Sets status + note from value, age and limits; range = the normal band.
void evaluate(Gauge &g, const Limits &l, uint32_t staleAfter_s);

}  // namespace panel
