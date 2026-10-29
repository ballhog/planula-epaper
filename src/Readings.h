// Direct-mode values: every input the Apex / Hydros reported, keyed
// "apex:<name>" or "hydros:<thing>/<name>", with a 24 h sparkline per key.
// Thread-safe: the Hydros MQTT task writes from its own task.
#pragma once
#include <Arduino.h>

#include <vector>

#include "Model.h"

namespace readings {

struct Reading {
  String key;     // stable id used by GaugeCfg.key
  String source;  // "Apex" / "Hydros"
  String device;  // controller name
  String name;    // input name as the controller calls it
  String kind;    // ph | temp | orp | cond | alk | calc | mag | level | other
  String unit;    // best guess for display ("°C", "mV", "ppt", "dKH", "ppm")
  float value = NAN;
  uint32_t atMs = 0;  // millis() of the last update
  // 24 h trend: one point per 30 min (the mean of what arrived in that slot).
  float spark[panel::kSparkLen];
  uint8_t nspark = 0;
  float slotSum = 0;
  uint16_t slotN = 0;
  uint32_t slotStartMs = 0;
};

void update(const String &key, const String &source, const String &device, const String &name, const String &kind,
            const String &unit, float value);
// Copy of one reading; false if unknown.
bool get(const String &key, Reading &out);
// Snapshot of everything (for the picker on the settings page).
std::vector<Reading> all();
void clearSource(const String &source);

// Kind from a controller's type/name ("Tmp" → temp, "Salt" → cond, ...).
String guessKind(const String &type, const String &name);
String unitFor(const String &kind);

}  // namespace readings
