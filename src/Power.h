// Battery power modes (reTerminal E1001). Settings → Display → Power:
//
//   live   always on, as on wall power (and the only mode on the Waveshare board)
//   saver  deep sleep; wake every N minutes, fetch, and redraw only when a value
//          moved more than a step (pH 0.02, 0.1 °C, 0.1 ppt, 5 mV ...) or a
//          status changed, and at least hourly
//   daily  one update a day at a set time, plus an optional hourly alarm check
//          that redraws only when a status changed
//
// Any button wakes the panel: fresh readings, the settings page answers, and it
// stays awake 2 min after the last button press or web request. In battery modes
// the screen shows "Updated 7:02" instead of a running clock.
#pragma once
#include <Arduino.h>

#include "Model.h"

namespace power {

void begin();       // after settings::load(), before the first draw
void loop();
bool sleepy();      // a battery mode is on (reTerminal only)
bool quiet();       // timer wake: no drawing until power decides
void touch();       // a button or web request: stay awake a while
String nextWakeText();  // "8:00" ("" when live)
String status();    // for the status card
// Battery modes: mark the gauges that moved since the screen was last drawn.
void markMoved(panel::Page &p);

}  // namespace power
