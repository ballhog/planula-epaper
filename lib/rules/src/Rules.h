// Pure decision logic, kept free of Arduino so the host tests can run it
// (tools/host/render.sh). The firmware modules call these:
//   battery redraws and wake times  -> Power.cpp
//   outlet history and its lanes    -> Outlets.cpp
//   controller output states        -> Apex.cpp, Hydros.cpp
#pragma once
#include <stdint.h>
#include <time.h>

#include "Model.h"

namespace rules {

// Elapsed ms from `then` to `now`, signed. A timestamp written by another task
// can be a little newer than the `now` read before it; unsigned, that wraps to
// ~49 days (build 17 dropped every new Hydros session as "expired").
inline int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

// ── battery modes ──────────────────────────────────────────────────────────

// How much a value must move before a battery-mode redraw.
float redrawStep(const panel::Gauge &g);

// What the screen showed, kept through deep sleep. Plain data, no default
// member values: it lives in RTC_DATA_ATTR memory, and a constructor would run
// on every wake and wipe it (seen in review of build 56: every battery wake
// then counted as "moved" and redrew).
struct Drawn {
  uint8_t n, page;
  float vals[panel::kMaxGauges];
  uint8_t stat[panel::kMaxGauges];
};
void remember(const panel::Page &p, uint8_t page, Drawn &d);
// moved: a value moved by its step, or appeared / went missing.
// statusChanged: any gauge's status differs. A different page counts as both.
void compare(const panel::Page &p, uint8_t page, const Drawn &d, bool &moved, bool &statusChanged);

enum class Wake : uint8_t { None, Saver, Daily, Hourly };
// Saver: on movement, a status change, or an hour since the last draw.
// Daily: always. Hourly check: only on a status change.
bool shouldRedraw(Wake why, bool moved, bool statusChanged, uint32_t secondsSinceDraw);

// Next local time today/tomorrow at hh:mm (more than 30 s ahead).
time_t nextAt(time_t now, int hh, int mm);
// When to wake next and why. mode: 's' saver, 'd' daily. Without a valid
// clock: an hour, as a saver wake.
time_t nextWake(time_t now, bool clockValid, char mode, int saverMin, int dailyH, int dailyM, bool hourly, Wake &why);

// ── outlets ────────────────────────────────────────────────────────────────

constexpr int kHist = 46;

// Level changes of one output (0-100 %), times in ms. Steps under 5 % are
// dropped (except to or from off) so a ramp doesn't fill the buffer.
// Plain data, no default member values: the firmware keeps it in RTC memory so
// it survives battery sleeps (zero it with `= {}` or memset). `at` is a wrapping
// millisecond clock that must keep counting through sleep (wall clock).
struct OutletHistory {
  uint8_t len;
  uint32_t at[kHist];
  uint8_t v[kHist];
  uint16_t ml10[kHist];  // dosing heads: amount of the dose starting here, 0.1 mL (0 unknown)
  void record(uint32_t ms, uint8_t level);
  // A dose from a dosing log: a start (with its amount) and an end a second later.
  void recordDose(uint32_t ms, float ml);
};

// Timeline points for a lane covering `fromMin` (negative) up to now.
// doser: a point per dose start (value NaN); variable: levels; else 0/1.
// Writes up to `max` points; returns how many.
int lanePoints(const OutletHistory &h, uint32_t nowMs, int fromMin, bool doser, bool variable, int16_t *t, float *val,
               int max);

// Hydros Output entry -> level 0-100. valueState is 0..10000; the water-change
// pair (drain/fill) and dosing heads are on/off. NaN = field absent.
uint8_t hydrosLevel(float valueState, float drain, float fill, bool doser);

// Apex output status[0] / type -> on, by hand, hidden from the picker.
void apexOutput(const char *status0, const char *type, bool &on, bool &manual, bool &hidden);

// ── controller history (Apex /rest/ilog, /rest/olog) ──────────────────────

// An Apex log date as epoch seconds: a number is already epoch (AOS 5.x); a
// "YYYYMMDDHHMM" string (classic firmware) is local time. 0 = unreadable.
uint32_t apexDate(double num, const char *str);

// Which 30-min slot of the 24 h ending at nowS a time falls in: 0 = oldest,
// slots-1 = the slot that ends at now. -1 when outside.
int slotFor(uint32_t t, uint32_t nowS, int slots, uint32_t slotS);

// Slot means (sum / n) -> sparkline points, oldest first. Leading empty slots
// are dropped (the log may start later); empty slots after the first carry the
// previous value forward. Returns the number of points.
int slotPoints(const float *sum, const uint16_t *n, int slots, float *out);

// "ON" / "AON" / "TBL" ... -> on (Apex output log states).
bool apexOn(const char *state);

}  // namespace rules
