// Controller outputs (Apex outlets; Hydros once its format is known) and the
// on/off history of the ones picked for the Outlets page.
//
// Every output a controller reports is listed (settings page → Outlets) with
// its live state; the ones the reader picks get a timeline page of when they
// were on, up to now. Direct mode only knows the past: the controllers don't
// share their programs. (In Coeno mode the Schedule page shows the future too.)
#pragma once
#include <Arduino.h>

#include "Model.h"

namespace outlets {

constexpr int kMax = 40;      // outputs listed
constexpr int kPicked = 8;    // outputs with history (timeline lanes)
constexpr int kHist = 46;     // state changes kept per picked output

struct Out {
  char key[44];     // "apex:Return"
  char source[8];   // "Apex"
  char type[10];    // Apex: outlet | 24v | variable | virtual | alert ...
  bool on;
  bool manual;      // forced on/off by hand (Apex ON/OFF, not AON/AOF)
  bool hidden;      // virtual / alert outputs: not offered by default
  uint8_t level;    // 0-100 % (on/off outputs: 0 or 100)
  bool variable;    // has reported a level between 0 and 100 (lights, pumps)
  uint32_t atMs;    // last report
};

// level: 0-100 % (pass 100 / 0 for plain on/off outputs).
void update(const char *key, const char *source, const char *type, uint8_t level, bool manual, bool hidden);
int count();
bool at(int i, Out &out);
bool get(const String &key, Out &out);
const char *nameOf(const char *key);  // the part after "source:"

// The Outlets page as timeline lanes, 12 h up to now. Lane per output kind:
// dosing heads = a mark per dose, dimmable outputs = their level (%),
// everything else on/off bars. Uses settings' picks for order and labels.
bool buildPage(panel::Page &p);

}  // namespace outlets
