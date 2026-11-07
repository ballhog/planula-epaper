#include "Power.h"

#include "Settings.h"

#if defined(BOARD_RETERMINAL)

#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <math.h>
#include <time.h>

#include <type_traits>

#include "Apex.h"
#include "Board.h"
#include "App.h"
#include "CoenoLink.h"
#include "Display.h"
#include "Hydros.h"
#include "Model.h"
#include "Net.h"
#include "Readings.h"
#include "Rules.h"
#include "Worker.h"

namespace power {

namespace {

constexpr uint32_t kMagic = 0x504C4E31;  // "PLN1"
constexpr int kKeep = 12;                // readings carried across sleep
constexpr uint32_t kAwakeKeyMs = 120000, kAwakeBootMs = 300000;
constexpr uint64_t kWakeMask = (1ULL << 3) | (1ULL << 4) | (1ULL << 5);  // the three keys
using rules::Wake;

// Kept in RTC memory through deep sleep (lost on power-off or reset).
struct Rtc {
  uint32_t magic;
  uint32_t savedAt;       // epoch when it went to sleep
  uint32_t drawnAt;       // epoch of the readings on screen
  Wake next;              // why the timer will wake it
  uint8_t page;
  bool acked;
  uint32_t ackSig;
  rules::Drawn drawn;     // what the screen shows
  uint8_t nr;
  readings::Reading r[kKeep];  // times stored as ages (ms) at savedAt
};
RTC_DATA_ATTR Rtc rtc;
// No constructor may touch it: one would run on every wake and wipe what the
// sleep was meant to keep.
static_assert(std::is_trivially_default_constructible<Rtc>::value, "Rtc must be plain data (RTC memory)");

bool quietCycle = false;  // timer wake: decide, maybe draw, sleep
uint32_t awakeUntil = 0, cycleStart = 0;
bool sleeping = false;
int pendingPage = -1;

String hhmm(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[8];
  if (settings::get().clock24) strftime(b, sizeof b, "%H:%M", &tm);
  else snprintf(b, sizeof b, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
  return b;
}

void saveState() {
  const Settings &s = settings::get();
  rtc.magic = kMagic;
  rtc.savedAt = time(nullptr);
  rtc.page = app::page();
  rtc.acked = app::alarmAcked();
  rtc.ackSig = app::alarmSig();
  // The readings the gauges use, so values and trends survive the sleep.
  rtc.nr = 0;
  uint32_t now = millis();
  for (auto &g : s.gauges) {
    if (rtc.nr >= kKeep) break;
    readings::Reading r;
    if (!readings::get(g.key, r)) continue;
    bool dup = false;
    for (int i = 0; i < rtc.nr; i++) dup |= !strcmp(rtc.r[i].key, r.key);
    if (dup) continue;
    r.atMs = now - r.atMs;  // ages
    r.slotStartMs = now - r.slotStartMs;
    rtc.r[rtc.nr++] = r;
  }
}

void restoreState() {
  if (rtc.magic != kMagic) return;
  uint32_t slept = 0;
  time_t now = time(nullptr);
  if (now > (time_t)rtc.savedAt) slept = (uint32_t)(now - rtc.savedAt) * 1000UL;
  uint32_t ms = millis();
  for (int i = 0; i < rtc.nr && i < kKeep; i++) {
    readings::Reading r = rtc.r[i];
    r.atMs = ms - (r.atMs + slept);
    r.slotStartMs = ms - (r.slotStartMs + slept);
    readings::restore(r);
  }
  pendingPage = rtc.page;  // applied in loop(), once the page count is known
}

void markMoved(panel::Page &p) {
  if (!sleepy() || rtc.magic != kMagic || !rtc.drawnAt) return;
  rules::markMoved(p, (uint8_t)app::page(), rtc.drawn);
}

void remember(const panel::Page &p) {
  rules::remember(p, (uint8_t)app::page(), rtc.drawn);
  rtc.drawnAt = time(nullptr);
}

bool dataReady() {
  const Settings &s = settings::get();
  if (s.mode == "coeno") return coeno::lastPageMs() != 0;
  bool a = !s.apexOn || apex::lastOkMs() != 0;
  bool h = !s.hydrosOn || hydros::ok();
  return a && h;
}

void drawAndRemember() {
  app::drawNow();  // full refresh: first draw since wake
  panel::Page *p = new (std::nothrow) panel::Page();
  if (p) {
    if (app::currentPage(*p)) remember(*p);
    delete p;
  }
}

void goToSleep() {
  const Settings &s = settings::get();
  time_t now = time(nullptr);
  bool clock = net::timeValid();
  Wake why;
  int dh = s.dailyAt.substring(0, 2).toInt(), dm = s.dailyAt.substring(3, 5).toInt();
  time_t at = rules::nextWake(now, clock, s.power == "saver" ? 's' : 'd', s.saverMin, dh, dm, s.hourlyCheck, why);
  uint32_t secs = (uint32_t)max<time_t>(30, at - now);
  rtc.next = why;
  saveState();
  Serial.printf("[power] sleeping %u s (%s, wake %s)\n", (unsigned)secs,
                why == Wake::Daily ? "daily update" : why == Wake::Hourly ? "hourly alarm check" : "saver",
                clock ? hhmm(at).c_str() : "?");
  Serial.flush();
  display::sleep();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  for (int pin : {3, 4, 5}) {
    rtc_gpio_pullup_en((gpio_num_t)pin);
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
  }
  esp_sleep_enable_ext1_wakeup(kWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

bool busy() {
  // Never sleep in the middle of an update or a network job.
  return worker::running()[0] || worker::pending("update") || app::updating();
}

}  // namespace

bool sleepy() { return settings::get().power != "live"; }
bool quiet() { return sleepy() && quietCycle; }

void touch() {
  // Also in live mode: switching to a battery mode from the page then gives
  // the usual 2 min before the first sleep.
  if (quietCycle && sleepy()) {  // a button during a timer wake: become an awake window
    quietCycle = false;
    app::redraw(true);
  }
  awakeUntil = millis() + kAwakeKeyMs;
}

void begin() {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  bool fromSleep = c == ESP_SLEEP_WAKEUP_TIMER || c == ESP_SLEEP_WAKEUP_EXT1;
  // Local time before Wi-Fi: the RTC kept the clock through deep sleep.
  setenv("TZ", settings::get().tz.c_str(), 1);
  tzset();
  if (fromSleep) restoreState();
  else rtc.magic = 0;
  for (int pin : {3, 4, 5}) rtc_gpio_deinit((gpio_num_t)pin);  // back to plain inputs for the keys
  if (c == ESP_SLEEP_WAKEUP_TIMER && sleepy()) {
    quietCycle = true;
    cycleStart = millis();
  } else {
    awakeUntil = millis() + (c == ESP_SLEEP_WAKEUP_EXT1 ? kAwakeKeyMs : kAwakeBootMs);
  }
  Serial.printf("[power] %s, woke by %s\n", settings::get().power.c_str(),
                c == ESP_SLEEP_WAKEUP_TIMER ? "timer" : c == ESP_SLEEP_WAKEUP_EXT1 ? "a button" : "power-on/reset");
}

void loop() {
  if (pendingPage >= 0) {
    if (pendingPage < app::pages()) app::gotoPage(pendingPage);
    pendingPage = -1;
  }
  if (!sleepy() || sleeping) return;
  uint32_t now = millis();
  if (quietCycle) {
    const Settings &s = settings::get();
    uint32_t limit = (s.mode == "direct" && s.hydrosOn) ? 60000 : 25000;
    bool timedOut = now - cycleStart > limit;
    if (!dataReady() && !timedOut) return;
    if (busy()) return;
    app::syncAlarms(rtc.acked, rtc.ackSig);
    panel::Page *p = new (std::nothrow) panel::Page();
    bool draw = true;
    if (p && app::currentPage(*p)) {
      bool moved, statusChanged;
      if (rtc.magic == kMagic) rules::compare(*p, (uint8_t)app::page(), rtc.drawn, moved, statusChanged);
      else moved = statusChanged = true;
      uint32_t sinceDraw = rtc.drawnAt ? (uint32_t)(time(nullptr) - rtc.drawnAt) : UINT32_MAX;
      draw = rules::shouldRedraw(rtc.next, moved, statusChanged, sinceDraw);
      Serial.printf("[power] %s wake: %s%s%s -> %s\n",
                    rtc.next == Wake::Daily ? "daily" : rtc.next == Wake::Hourly ? "hourly" : "saver",
                    moved ? "values moved " : "", statusChanged ? "status changed " : "",
                    timedOut ? "(timed out waiting for data) " : "", draw ? "redraw" : "no redraw");
    }
    delete p;
    // Offline: keep the last picture (its footer says when it's from) rather
    // than putting "Joining Wi-Fi" on the wall until the next wake.
    if (!net::online()) draw = false;
    if (draw) drawAndRemember();
    // A new or still unacknowledged alarm: beep before sleeping again. (A
    // button press during the beep acknowledges it and keeps it awake.)
    if (draw && app::hasUnackedAlarm()) {
      board::alarmBeep();
      uint32_t t0 = millis();
      while (board::beeping() && millis() - t0 < 2000) {
        board::loop();
        delay(5);
      }
    }
    sleeping = true;
    goToSleep();
    return;
  }
  if ((int32_t)(now - awakeUntil) < 0 || busy()) return;
  // End of an awake window: leave fresh readings on the screen, then sleep.
  if (net::online()) drawAndRemember();
  sleeping = true;
  goToSleep();
}

String nextWakeText() {
  if (!sleepy() || !net::timeValid()) return "";
  const Settings &s = settings::get();
  time_t now = time(nullptr);
  if (s.power == "saver") return hhmm(now + (time_t)s.saverMin * 60);
  return hhmm(rules::nextAt(now, s.dailyAt.substring(0, 2).toInt(), s.dailyAt.substring(3, 5).toInt()));
}

String status() {
  const Settings &s = settings::get();
  if (!sleepy()) return "live (always on)";
  String o = s.power == "saver" ? "saver, every " + String(s.saverMin) + " min" : "daily at " + s.dailyAt;
  if (s.power == "daily" && s.hourlyCheck) o += " + hourly alarm check";
  if (!quietCycle && awakeUntil) {
    int32_t left = (int32_t)(awakeUntil - millis()) / 1000;
    if (left > 0) o += " · awake " + String(left) + " s more";
  }
  return o;
}

}  // namespace power

#else  // Waveshare board: always live

namespace power {
void begin() {}
void loop() {}
bool sleepy() { return false; }
bool quiet() { return false; }
void touch() {}
String nextWakeText() { return ""; }
String status() { return "live (always on)"; }
void markMoved(panel::Page &) {}
}  // namespace power

#endif
