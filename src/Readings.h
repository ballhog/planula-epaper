// Direct-mode values: every input the Apex / Hydros reported, keyed
// "apex:<name>" or "hydros:<thing>/<name>", with a 24 h sparkline per key.
//
// Fixed-size records in one static table: no heap allocation per reading or
// per update. (Build 10 kept six Strings per reading in a growing vector;
// those small blocks, allocated while the first Hydros TLS session held its
// buffers, fragmented the heap so no later TLS session could be set up.)
// Thread-safe: the Hydros MQTT task writes from its own task.
#pragma once
#include <Arduino.h>

#include "Model.h"

namespace readings {

constexpr int kMax = 48;

struct Reading {
  char key[64];     // stable id used by GaugeCfg.key
  char source[8];   // "Apex" / "Hydros"
  char device[28];  // controller name
  char name[32];    // input name as the controller calls it
  char kind[8];     // ph | temp | orp | cond | alk | calc | mag | level | switch | other
  char unit[8];     // best guess for display ("°C", "mV", "ppt", "dKH", "ppm")
  float value;
  uint32_t atMs;    // millis() of the last update
  // 24 h trend: one point per 30 min (the mean of what arrived in that slot).
  float spark[panel::kSparkLen];
  uint8_t nspark;
  float slotSum;
  uint16_t slotN;
  uint32_t slotStartMs;
};

void update(const String &key, const char *source, const String &device, const String &name, const String &kind,
            const String &unit, float value);
// Copy of one reading; false if unknown.
bool get(const String &key, Reading &out);
int count();
bool at(int i, Reading &out);
void clearSource(const char *source);

// Kind from a controller's type/name ("Tmp" → temp, "Salt" → cond, ...).
String guessKind(const String &type, const String &name);
String unitFor(const String &kind);

}  // namespace readings
