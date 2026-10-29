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

// 64 inputs: an Apex with a couple of EnergyBars reports 30-50 (amps and watts
// per outlet) next to the 16 a Hydros sends. Build 14 had 48 and filled up.
constexpr int kMax = 64;

struct Reading {
  char key[48];     // stable id used by GaugeCfg.key
  char source[8];   // "Apex" / "Hydros"
  char device[24];  // controller name
  char name[28];    // input name as the controller calls it
  char kind[8];     // ph | temp | orp | cond | alk | calc | mag | level | switch | other
  char unit[8];     // best guess for display ("°C", "mV", "ppt", "dKH", "ppm")
  float value;
  uint32_t atMs;    // millis() of the last update
  // 24 h trend: one point per 30 min (the mean of what arrived in that slot),
  // stored as IEEE half floats (3 significant digits: plenty for a sparkline,
  // and half the RAM). Read with sparkAt().
  uint16_t spark[panel::kSparkLen];
  uint8_t nspark;
  float slotSum;
  uint16_t slotN;
  uint32_t slotStartMs;
};

float sparkAt(const Reading &r, int i);
// Counts up whenever a key gets its first value. The loop redraws when it
// changes, so a gauge waiting on "—" fills in at once instead of at the next
// minute (Hydros data arrived ~20 s after boot but showed up to a minute later).
uint32_t newKeys();
// Readings the table had no room for since boot.
uint32_t dropped();

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
