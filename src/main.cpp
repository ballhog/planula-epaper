// Planula-ePaper — Corallite wireless gauge panel.
// Waveshare e-Paper ESP32 Driver Board Rev 3 + 7.5" B/W panel (800x480).
//
// Modes (settings page): coeno = Coeno sends finished pages over MQTT;
// direct = the panel reads the Apex and/or Hydros itself. Firmware updates
// come from Coeno in both (pull OTA, SHA-256, rollback).
//
// BOOT button: short press = acknowledge alarm, else next page
//              hold 3 s  = info screen (IP, settings address, sources)
//              hold 10 s = open the setup Wi-Fi network
//              hold 25 s = erase all settings
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>

#include <memory>
#include <new>
#include <vector>

#include "Apex.h"
#include "App.h"
#include "CoenoLink.h"
#include "DirectPages.h"
#include "Display.h"
#include "Https.h"
#include "Hydros.h"
#include "Net.h"
#include "Readings.h"
#include "Render.h"
#include "Settings.h"
#include "Web.h"
#include "Worker.h"
#include "Board.h"
#include "Power.h"
#include "ota.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const int PIN_BUTTON = 0;  // BOOT

// Pages (a few kB each) and the renderer live on the loop task's stack.
// Network calls run on the worker task (12 kB, Worker.cpp); loop() only draws
// and serves the page. The [mem] line logs how much of each stack is unused.
#if defined(BOARD_RETERMINAL)
// The S3 build used all but ~670 bytes of 8 kB on its first run (2026-11-06).
// It has RAM to spare, so give it 16 kB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#else
SET_LOOP_TASK_STACK_SIZE(8 * 1024);
#endif

namespace {

int curPage = 0, nPages = 1;
uint32_t lastRotate = 0, lastDraw = 0, lastState = 0, infoUntil = 0, lastReminder = 0;
volatile bool redrawAsked = true;  // also set from the worker
bool fullAsked = false, identifyAsked = false;
bool healthy = false;

// Alarms across all pages.
struct Alarm {
  String id, text;
};
std::vector<Alarm> alarms;
std::vector<String> knownAlarmIds;
bool acked = false;
bool flashAsked = false;

// Updates.
// Checks and installs run on the worker task (Worker.h); these are shared.
volatile long availBuild = 0;
SharedText updateText("not checked yet");
volatile const char *updatingFrom = nullptr;  // set while a download runs: draw() shows it
volatile int updatePct = -1;          // GitHub download progress (Coeno: ota_download_pct())
volatile bool checking = false;       // a check is running on the worker
volatile bool updateFailed = false;   // an install just failed: open the System screen
volatile uint32_t checkedAtMs = 0;    // last finished check
String availSource, availUrl, availSha;  // where the available build is (worker writes, then sets availBuild)
size_t availSize = 0;
volatile bool autoInstallReady = false;
uint32_t lastCheck = 0;
volatile bool retrySoon = false;  // last check couldn't connect
bool checkAsked = false, installAsked = false, rebootAsked = false, needsReboot = false;
uint32_t rebootAt = 0;

String clockText() {
  if (!net::timeValid()) return "";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char b[8];
  if (settings::get().clock24) strftime(b, sizeof b, "%H:%M", &t);
  else snprintf(b, sizeof b, "%d:%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min);
  return b;
}

bool coenoMode() { return settings::get().mode == "coeno"; }
// Coeno is linked for firmware (either mode): only then does the panel offer
// updates on screen (System screen, "update ready" prompt, press to install).
bool coenoLinked() {
  const Settings &s = settings::get();
  return s.coenoUpdates && s.coenoHost.length();
}

int countPages() { return coenoMode() ? coeno::pageCount() : direct::pageCount(); }

// Fill page i with fresh data (Coeno: received page + local staleness).
bool fetchPage(int i, panel::Page &p) {
  p.h12 = !settings::get().clock24;
  if (!coenoMode()) {
    direct::build(i, p);
    p.view = panel::viewFromName(settings::get().view.c_str());
    if (power::sleepy())  // ages would freeze on screen: the footer gives the time
      for (int k = 0; k < p.n; k++) p.g[k].age_s = panel::kNoAge;
    return true;
  }
  uint32_t at;
  if (!coeno::getPage(i, p, at)) return false;
  p.view = panel::viewFromName(settings::get().view.c_str());
  p.h12 = !settings::get().clock24;
  uint32_t pageAge = (millis() - at) / 1000;
  panel::shiftTimeline(p, pageAge / 60);  // "now" moves on between Coeno's pages
  uint16_t staleAfter = settings::get().stale_s;
  for (int k = 0; k < p.n; k++) {
    panel::Gauge &g = p.g[k];
    g.age_s += pageAge;
    if (g.has && g.age_s > staleAfter && g.status != panel::Status::Alarm) g.status = panel::Status::Stale;
    if (power::sleepy()) g.age_s = panel::kNoAge;
  }
  return true;
}

void collectAlarms() {
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

void fillChrome(panel::Chrome &c) {
  const Settings &s = settings::get();
  PANEL_COPY(c.clock, power::sleepy() ? "" : clockText().c_str());  // no frozen clock on battery
  c.page = curPage;
  c.pages = nPages;
  if (!alarms.empty()) {
    PANEL_COPY(c.alarm, alarms[0].text.c_str());
    c.alarmCount = alarms.size();
    c.alarmAcked = acked;
  }
  String left, right;
  bool alert = false;
  if (coenoMode()) {
    uint32_t lp = coeno::lastPageMs();
    uint32_t age = lp ? (millis() - lp) / 1000 : 0;
    if (!s.coenoHost.length()) {
      left = "Standalone · no Coeno address set";
    } else if (!coeno::connected()) {
      left = "Coeno offline · showing the last pages";
      alert = true;
    } else if (lp && age > s.stale_s) {
      char a[16];
      panel::formatAge(age, a, sizeof a);
      left = String("STALE · no page from Coeno for ") + a;
      alert = true;
    } else {
      left = "Coeno · " + s.coenoHost;
      String ck = clockText();
      if (ck.length()) left += " · updated " + ck;
    }
  } else {
    if (s.apexOn) left += String("Apex ") + (apex::ok() ? "ok" : "offline");
    if (s.hydrosOn) left += String(left.length() ? " · " : "") + "Hydros " + (hydros::ok() ? "ok" : "offline");
    if (!left.length()) left = "Direct mode: no source switched on";
    alert = (s.apexOn && !apex::ok() && (int32_t)(millis() - apex::lastOkMs()) > (int32_t)(s.stale_s * 1000UL)) ||
            (s.hydrosOn && !hydros::ok() && millis() > 180000UL);
  }
  // Wi-Fi and battery are drawn as icons (Chrome.wifi / .battery).
  if (net::online()) {
    int r = net::rssi();
    c.wifi = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
  } else {
    c.wifi = 0;
  }
  c.battery = board::batteryPct();
  if (power::sleepy()) {
    // The screen is a snapshot: say when it was taken and when the next one is.
    String at = clockText(), nx = power::nextWakeText();
    String head = at.length() ? "Readings from " + at : String("Readings");
    if (nx.length()) head += " · next " + nx;
    left = head + " · " + left;
  }
  if (availBuild > 0 && coenoLinked()) right = "update ready, hold 3 s";
  PANEL_COPY(c.footerLeft, left.c_str());
  PANEL_COPY(c.footerRight, right.c_str());
  c.footerAlert = alert;
}

// A message screen as a scene that owns its text.
display::Scene message(const String &title, const String &body, const String &big, const panel::Chrome &c) {
  return [title, body, big, c](Adafruit_GFX &d) { panel::renderMessage(d, title.c_str(), body.c_str(), big.c_str(), c); };
}

// Info screen without Coeno: address and source status only, no update offers.
display::Scene drawInfo(panel::Chrome &c) {
  const Settings &s = settings::get();
  String body = "Settings:  http://" + s.name + ".local  or  http://" + net::ip() + "\n";
  body += "Mode: " + s.mode + "   ·   " + net::statusText() + "\n";
  if (s.apexOn) body += "Apex: " + apex::status() + "\n";
  if (s.hydrosOn) body += "Hydros: " + hydros::status() + "\n";
  body += "Build " + String(FW_BUILD) + " (" FW_VERSION ")";
  return message(s.name, body, net::ip(), c);
}

// System screen, Coeno linked (button held 3 s, an install running, or one that just failed):
// firmware update state on the left, panel details on the right.
display::Scene drawSystem(panel::Chrome &c) {
  const Settings &s = settings::get();
  struct Text {  // owns the strings the SystemInfo points at
    String version, from, detail, checked, hint, rows[8][2];
  };
  auto t = std::make_shared<Text>();
  auto si = std::make_shared<panel::SystemInfo>();
  long avail = availBuild;
  String detail = updateText.get();
  si->build = FW_BUILD;
  si->availBuild = avail;
  t->version = FW_VERSION;
  t->from = availSource == "github" ? "GitHub" : "Coeno";
  if (const char *from = (const char *)updatingFrom) {
    si->state = panel::SystemInfo::Installing;
    t->from = from;
    int pct = !strcmp(from, "GitHub") ? updatePct : ota_download_pct();
    si->pct = pct;
  } else if (checking) {
    si->state = panel::SystemInfo::Checking;
  } else if (detail.startsWith("update failed")) {
    si->state = panel::SystemInfo::Failed;
    t->checked = "still on build " + String(FW_BUILD) + " · nothing changed";
    t->hint = avail > 0 ? "Press: try again  ·  hold 3 s: re-check" : "Press: close  ·  hold 3 s: check again";
  } else if (avail > 0) {
    si->state = panel::SystemInfo::Available;
    t->hint = "Press the button to update";
  } else if (detail.startsWith("up to date")) {
    si->state = panel::SystemInfo::UpToDate;
  }
  t->detail = si->state == panel::SystemInfo::Checking ? String("asking Coeno") + (s.githubUpdates ? " / GitHub" : "") : detail;
  if (!t->checked.length() && checkedAtMs) {
    char a[16];
    panel::formatAge((millis() - checkedAtMs) / 1000, a, sizeof a);
    t->checked = String("checked ") + (strcmp(a, "now") ? String(a) + " ago" : String("just now"));
  }
  if (!t->hint.length()) t->hint = "Press: close  ·  hold 3 s: check now";
  if (s.autoUpdate && si->state == panel::SystemInfo::Available) t->hint = "Installs by itself (auto) · or press";

  uint32_t up = millis() / 1000;
  char upt[24];
  if (up >= 86400) snprintf(upt, sizeof upt, "%u d %u h", (unsigned)(up / 86400), (unsigned)(up % 86400 / 3600));
  else snprintf(upt, sizeof upt, "%u h %u min", (unsigned)(up / 3600), (unsigned)(up % 3600 / 60));
  int n = 0;
  auto row = [&](const String &k, const String &v) {
    if (n < 8) { t->rows[n][0] = k; t->rows[n][1] = v; n++; }
  };
  row("ADDRESS", s.name + ".local");
  row("", net::ip());
  row("MODE", coenoMode() ? "Coeno" : "Direct");
  row("WI-FI", net::statusText());
  if (coenoMode()) row("COENO", coeno::status());
  else {
    if (s.apexOn) row("APEX", apex::status());
    if (s.hydrosOn) row("HYDROS", hydros::status());
  }
  row("UPTIME", upt);
  for (int i = 0; i < n; i++) {
    si->keys[i] = t->rows[i][0].c_str();
    si->vals[i] = t->rows[i][1].c_str();
  }
  si->nrows = n;
  si->version = t->version.c_str();
  si->availFrom = t->from.c_str();
  si->detail = t->detail.c_str();
  si->checked = t->checked.c_str();
  si->hint = t->hint.c_str();
  return [t, si, c](Adafruit_GFX &d) { panel::renderSystem(d, *si, c); };
}

// What kind of screen is up ("setup", "info", "page:2", ...). Partial refreshes
// leave a faint trace of what was there before, which shows when the whole
// layout changes (seen on the first panel: old message text behind the
// gauges). So a change of screen gets one full refresh; value updates on the
// same screen stay partial.
String lastScreen;

void draw() {
  String screen;
  panel::Chrome c;
  fillChrome(c);
  const Settings &s = settings::get();
  display::Scene scene;

  if (const char *from = (const char *)updatingFrom) {
    if (coenoLinked()) scene = drawSystem(c);
    else
      scene = message("Updating firmware",
                      String("Downloading from ") + from + ". Don't unplug the panel.\nIt restarts by itself when done.",
                      "", c);
    screen = "updating";
  } else if (net::state() == net::State::SetupAp && !net::online()) {
    String ap = net::apName(), pass = s.apPass, url = "http://192.168.4.1", why = net::setupReason();
    String left = s.name + " · setup mode";
    PANEL_COPY(c.footerLeft, left.c_str());
    c.footerAlert = false;
    snprintf(c.footerRight, sizeof c.footerRight, "build %d", (int)FW_BUILD);
    scene = [ap, pass, url, why, c](Adafruit_GFX &d) {
      panel::SetupInfo si;
      si.apName = ap.c_str();
      si.apPass = pass.c_str();
      si.url = url.c_str();
      si.reason = why.c_str();
      panel::renderSetup(d, si, c);
    };
    screen = "setup";
  } else if (millis() < infoUntil) {
    scene = coenoLinked() ? drawSystem(c) : drawInfo(c);
    screen = "info";
  } else if (!net::online()) {
    scene = message("Joining Wi-Fi",
                    "Looking for " + (s.wifi.empty() ? String("a network") : s.wifi[0].ssid) +
                        " …\nHold the BOOT button 10 s to open the setup network.",
                    "", c);
    screen = "joining";
  } else {
    // The page lives on the heap only while it's the current scene (~3 kB).
    std::shared_ptr<panel::Page> p(new (std::nothrow) panel::Page());
    if (!p || !fetchPage(curPage, *p)) {
      String body;
      String title;
      if (coenoMode() && !s.coenoHost.length()) {
        title = "Standalone";
        body = "Connected to Wi-Fi as " + net::ip() + ". No Coeno address set.\n" +
               "Set the Coeno address, or switch to direct mode, at\nhttp://" + s.name + ".local";
      } else if (coenoMode()) {
        title = "Waiting for Coeno";
        body = "Connected to Wi-Fi as " + net::ip() + ".\nCoeno: " + coeno::status() +
               ".\nAdd this panel in Coeno, or switch to direct mode at\nhttp://" + s.name + ".local";
      } else {
        title = "No gauges yet";
        body = "Pick what to show at http://" + s.name + ".local\n(or http://" + net::ip() + ")";
      }
      scene = message(title, body, net::ip(), c);
      screen = title;
    } else {
      scene = [p, c](Adafruit_GFX &d) { panel::render(d, *p, c); };
      // The view is part of the screen: switching it redraws with a full refresh.
      screen = "page:" + String(curPage) + ":" + String(p->n) + ":" + String(nPages) + ":" + panel::viewName(p->view);
    }
  }
  // A new build has proven itself once it is on Wi-Fi and drawing.
  if (!healthy && net::online()) {
    ota_mark_healthy();
    healthy = true;
  }
  bool full = fullAsked || screen != lastScreen;
  fullAsked = false;
  lastScreen = screen;
  display::show(scene, full);
  if (identifyAsked) {
    identifyAsked = false;
    display::flash(3);
  } else if (flashAsked) {
    flashAsked = false;
    display::flash(3);
  }
}

// What's wrong right now, for Coeno ("" = fine): the same things the footer
// turns black for, plus a failed update.
String faultText() {
  const Settings &s = settings::get();
  if (coenoMode()) {
    uint32_t lp = coeno::lastPageMs();
    if (s.coenoHost.length() && lp && (millis() - lp) / 1000 > s.stale_s) {
      char a[16];
      panel::formatAge((millis() - lp) / 1000, a, sizeof a);
      return String("no page from Coeno for ") + a;
    }
  } else {
    if (s.apexOn && !apex::ok() && (int32_t)(millis() - apex::lastOkMs()) > (int32_t)(s.stale_s * 1000UL))
      return "Apex: " + apex::status();
    if (s.hydrosOn && !hydros::ok() && millis() > 180000UL) return "Hydros: " + hydros::status();
  }
  String u = updateText.get();
  if (u.startsWith("update failed")) return u;
  return "";
}

// The chip's own temperature, where the sensor is worth reading (ESP32-S3,
// reTerminal). The classic ESP32's sensor is unreliable: left out.
float chipTempC() {
#if CONFIG_IDF_TARGET_ESP32S3
  float t = temperatureRead();
  if (isfinite(t) && t > -40 && t < 125) return t;
#endif
  return NAN;
}

void publishState() {
  JsonDocument d;
  d["page"] = curPage;
  d["pages"] = nPages;
  d["mode"] = settings::get().mode;
  d["build"] = FW_BUILD;
  d["available_build"] = availBuild;
  d["ip"] = net::ip();
  d["rssi"] = net::rssi();
  d["uptime_s"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["refreshes"] = display::refreshes();
  d["full_refreshes"] = display::fullRefreshes();
  JsonArray a = d["alarms"].to<JsonArray>();
  for (auto &al : alarms) a.add(al.id);
  d["acked"] = acked;
  if (settings::get().apexOn) d["apex"] = apex::status();
  if (settings::get().hydrosOn) d["hydros"] = hydros::status();
  d["fault"] = faultText();
  float ct = chipTempC();
  if (!isnan(ct)) d["chip_temp_c"] = roundf(ct * 10) / 10;
  d["coeno_login"] = coeno::loggedIn();
  String s;
  serializeJson(d, s);
  coeno::publishState(s);
}

// State goes to Coeno on connect, when something it shows changes (page,
// alarms, a refresh, a fault, an update), and every 60 s as a heartbeat.
// rssi / heap / uptime only ride along: they change all the time.
String stateSignature() {
  String sig = String(curPage) + "/" + nPages + "/" + settings::get().mode + "/" + availBuild + "/" + (acked ? 1 : 0) +
               "/" + display::refreshes() + "/" + display::fullRefreshes() + "/" + faultText() + "/";
  for (auto &al : alarms) sig += al.id + ",";
  return sig;
}

void stateOnChange() {
  static bool wasConnected = false;
  static String lastSig;
  static uint32_t lastCheckMs = 0, lastSentMs = 0;
  uint32_t now = millis();
  bool conn = coeno::connected();
  if (!conn) {
    wasConnected = false;
    return;
  }
  if (now - lastCheckMs < 500) return;
  lastCheckMs = now;
  String sig = stateSignature();
  bool fresh = !wasConnected;
  if (!fresh && (sig == lastSig || now - lastSentMs < 1000)) return;
  wasConnected = true;
  lastSig = sig;
  lastSentMs = now;
  lastState = now;  // the heartbeat counts from here
  publishState();
}

void onCoenoCommand(const String &leaf, const String &payload) {
  JsonDocument d;
  deserializeJson(d, payload);
  if (leaf == "next") app::nextPage();
  else if (leaf == "page") app::gotoPage(d["n"] | 0);
  else if (leaf == "refresh") app::redraw(d["full"] | false);
  else if (leaf == "identify") app::identify();
  else if (leaf == "check") app::checkUpdateSoon();
  else if (leaf == "update") app::installUpdateSoon();
  else if (leaf == "ack") { acked = true; board::silence(); app::redraw(false); }
}

#if !defined(BOARD_RETERMINAL)  // the reTerminal has its own keys, below
// ── button ──────────────────────────────────────────────────────────────
uint32_t pressedAt = 0;
bool wasDown = false, longDone = false;

void button() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !wasDown) {
    pressedAt = now;
    longDone = false;
  }
  if (down && !longDone) {
    uint32_t held = now - pressedAt;
    if (held > 25000) {
      longDone = true;
      settings::factoryReset();
      panel::Chrome c;
      display::show(message("Settings erased", "Restarting into setup mode…", "", c), true);
      delay(500);
      ESP.restart();
    }
  }
  if (!down && wasDown) {
    uint32_t held = now - pressedAt;
    if (held >= 10000 && held <= 25000) {
      net::openSetup("Setup network opened with the button.");
      app::redraw(false);
    } else if (held >= 3000) {
      // Opens the System screen; held again while it's up = check for updates now.
      if (infoUntil && now < infoUntil && coenoLinked()) app::checkUpdateSoon();
      infoUntil = now + 120000;
      app::redraw(false);
    } else if (held >= 40) {
      if (infoUntil && now < infoUntil) {
        // System screen: press = install when there's one (or retry), else close.
        if (availBuild > 0 && coenoLinked()) {
          app::installUpdateSoon();
          infoUntil = now + 120000;
        } else {
          infoUntil = 0;
        }
        app::redraw(false);
      } else if (!alarms.empty() && !acked) app::ackAlarm();
      else app::nextPage();
    }
  }
  wasDown = down;
}

#endif

#if defined(BOARD_RETERMINAL)
// ── reTerminal E1001 keys (Seeed: KEY0 GPIO3 right/green, KEY1 GPIO4 middle,
// KEY2 GPIO5 left; active low) ─────────────────────────────────────────────
//   left / right       previous / next page
//   middle             acknowledge the alarm, else switch the view
//                      (number + trend -> graphs -> numbers only)
//   middle hold 3 s    System screen (hold again there = check for updates)
//   middle hold 10 s   open the setup network
//   left + right 10 s  erase settings
struct Key {
  int pin;
  bool down = false, longDone = false;
  uint32_t at = 0, changed = 0;
};
Key keyRight{3}, keyMid{4}, keyLeft{5};

// Debounced: true when the key changed state this pass.
bool readKey(Key &k, uint32_t now) {
  bool d = digitalRead(k.pin) == LOW;
  if (d == k.down || now - k.changed < 30) return false;
  k.down = d;
  k.changed = now;
  if (d) {
    k.at = now;
    k.longDone = false;
  }
  return true;
}

void keysBegin() {
  pinMode(keyRight.pin, INPUT_PULLUP);
  pinMode(keyMid.pin, INPUT_PULLUP);
  pinMode(keyLeft.pin, INPUT_PULLUP);
}

void keys() {
  uint32_t now = millis();
  bool eR = readKey(keyRight, now), eM = readKey(keyMid, now), eL = readKey(keyLeft, now);
  if (eR || eM || eL) power::touch();
  if (eR || eM || eL)
    Serial.printf("[keys] left %s, middle %s, right %s\n", keyLeft.down ? "down" : "up", keyMid.down ? "down" : "up",
                  keyRight.down ? "down" : "up");

  // Left + right held together: erase.
  if (keyLeft.down && keyRight.down) {
    uint32_t both = now - max(keyLeft.at, keyRight.at);
    if (both > 10000) {
      settings::factoryReset();
      panel::Chrome c;
      display::show(message("Settings erased", "Restarting into setup mode…", "", c), true);
      delay(500);
      ESP.restart();
    }
    if (both > 300) keyLeft.longDone = keyRight.longDone = true;  // no page turn after a chord
    return;
  }

  bool infoUp = infoUntil && now < infoUntil;
  // Presses and 3 s holds do what Display → Buttons says. Fixed: middle held
  // 10 s = setup network, left + right held 10 s = erase.
  auto doAction = [&](const String &action) {
    if (action == "prev") app::prevPage();
    else if (action == "next") app::nextPage();
    else if (action == "view") app::cycleView();
    else if (action == "schedule") app::toggleSchedule();
    else if (action.startsWith("page")) app::togglePage(action.substring(4).toInt() - 1);
    else if (action == "refresh") {
      apex::pollNow();
      app::redraw(true);
    } else if (action == "info") {
      infoUntil = now + 120000;
      app::redraw(false);
    }
  };
  auto shortPress = [&](const String &action, bool isMiddle) {
    const Settings &s = settings::get();
    bool ackHere = s.keyAck == "any" || isMiddle;
    if (ackHere && !alarms.empty() && !acked) {
      app::ackAlarm();
      return;
    }
    if (infoUp) {  // on the System screen a press closes it (middle installs)
      if (isMiddle && availBuild > 0 && coenoLinked()) {
        app::installUpdateSoon();
        infoUntil = now + 120000;
      } else {
        infoUntil = 0;
      }
      app::redraw(false);
      return;
    }
    doAction(action);
  };
  auto holdPress = [&](const String &action) {
    if (action == "info" && infoUp) {  // held again on the System screen: check for updates
      if (coenoLinked() || settings::get().githubUpdates) app::checkUpdateSoon();
      infoUntil = now + 120000;
      app::redraw(false);
      return;
    }
    doAction(action);
  };
  const Settings &ks = settings::get();
  if (eL && !keyLeft.down && !keyLeft.longDone) {
    if (now - keyLeft.at >= 3000) holdPress(ks.holdLeft);
    else shortPress(ks.keyLeft, false);
  }
  if (eR && !keyRight.down && !keyRight.longDone) {
    if (now - keyRight.at >= 3000) holdPress(ks.holdRight);
    else shortPress(ks.keyRight, false);
  }
  if (eM && !keyMid.down && !keyMid.longDone) {
    uint32_t held = now - keyMid.at;
    if (held >= 10000) {
      net::openSetup("Setup network opened with the button.");
      app::redraw(false);
    } else if (held >= 3000) {
      holdPress(ks.holdMid);
    } else {
      shortPress(ks.keyMid, true);
    }
  }
}
#endif

// Network upload from the Mac: pio run -e epaper-net -t upload
// (password = CALYX_OTA_PASS at build time; empty = no password).
bool otaStarted = false;
void netUpload() {
#ifdef OTA_PASSWORD
  // No password compiled in (e.g. a public GitHub release build): no listener,
  // so nobody on the network can push firmware without it.
  if (!strlen(OTA_PASSWORD)) return;
#else
  return;
#endif
  if (!net::online()) return;
  if (!otaStarted) {
    ArduinoOTA.setHostname(settings::get().name.c_str());
    ArduinoOTA.setMdnsEnabled(false);  // Net.cpp already runs mDNS
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.onStart([] {
      Serial.println("[update] network upload started");
      panel::Chrome c;
      display::show(message("Updating firmware", "Receiving from the Mac. Don't unplug the panel.", "", c), false);
    });
    ArduinoOTA.onEnd([] {
      Serial.println("[update] network upload done");
      ota_mark_pending();
    });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[update] network upload failed (%d)\n", (int)e); });
    ArduinoOTA.begin();
    otaStarted = true;
  }
  ArduinoOTA.handle();
}

// Updates: Coeno first (LAN); when Coeno doesn't answer, GitHub releases
// (ballhog/planula-epaper, built by .github/workflows/release.yml). Either way
// SHA-256 checked, and the new build must prove itself or it rolls back.
// One manifest per firmware image: latest.json (ESP32 board), latest-s3.json (reTerminal).
#if defined(BOARD_RETERMINAL)
const char *kGithubManifest = "https://github.com/ballhog/planula-epaper/releases/latest/download/latest-s3.json";
#else
const char *kGithubManifest = "https://github.com/ballhog/planula-epaper/releases/latest/download/latest.json";
#endif

// Called on the worker: the loop draws the notice (only it touches the display).
void showUpdating(const char *from) {
  updatePct = -1;
  updatingFrom = from;
  redrawAsked = true;
  delay(3000);  // let the notice reach the screen before the download takes the CPU
}

void checkUpdates() {
  const Settings &s = settings::get();
  availBuild = 0;
  availSource = "";
  String coenoWhy;
  if (s.coenoUpdates && s.coenoHost.length()) {
    String url = "http://" + s.coenoHost + ":8000/firmware/" FW_IMAGE "/latest.json";
    long b = ota_check(url.c_str());
    if (b > 0) {
      availBuild = b;
      availSource = "coeno";
      updateText.set("build " + String(b) + " ready on Coeno");
      return;
    }
    if (b == 0 && ota_last_http() != 404) {
      updateText.set("up to date (build " + String(FW_BUILD) + ", Coeno)");
      return;
    }
    coenoWhy = String("Coeno: ") + ota_last_reason() + "; ";
  }
  if (!s.githubUpdates) {
    updateText.set(coenoWhy.length() ? coenoWhy + "GitHub updates off" : "update checks off");
    return;
  }
  JsonDocument m;
  https::Result r = https::requestJson("GET", kGithubManifest, {{"Accept", "application/json"}}, "", m, nullptr);
  if (r.status != 200 || r.error.length()) {
    updateText.set(coenoWhy + "GitHub: " + (r.status < 0 ? r.error : "HTTP " + String(r.status)));
    return;
  }
  long b = m["build"] | 0L;
  String sha = m["sha256"] | "";
  bool shaOk = sha.length() == 64;
  for (size_t i = 0; shaOk && i < sha.length(); i++) shaOk = isxdigit((unsigned char)sha[i]);
  if (b > FW_BUILD && m["url"].is<const char *>() && !shaOk) {
    // Never install a build that can't be checked.
    updateText.set(coenoWhy + "GitHub: build " + String(b) + " refused, manifest has no sha256");
  } else if (b > FW_BUILD && m["url"].is<const char *>()) {
    availBuild = b;
    availSource = "github";
    availUrl = m["url"].as<const char *>();
    availSha = sha;
    availSize = m["size"] | 0;
    updateText.set("build " + String(b) + " ready on GitHub");
  } else {
    updateText.set(coenoWhy + "up to date (build " + String(FW_BUILD) + ", GitHub)");
  }
}

void installUpdate() {
  if (availSource == "github") {
    showUpdating("GitHub");
    String e = https::downloadFirmware(availUrl, availSha, availSize, [](int pct) { updatePct = pct; });
    if (e.length()) {
      updatingFrom = nullptr;
      updateFailed = true;
      updateText.set("update failed: " + e);
      app::redraw(false);
      return;
    }
    ota_mark_pending();
    Serial.printf("[update] build %ld from GitHub installed, restarting\n", availBuild);
    delay(300);
    ESP.restart();
  }
  const Settings &s = settings::get();
  String url = "http://" + s.coenoHost + ":8000/firmware/" FW_IMAGE "/latest.json";
  showUpdating("Coeno");
  if (!ota_check_and_apply(url.c_str(), nullptr)) {
    updatingFrom = nullptr;
    updateFailed = true;
    updateText.set(String("update failed: ") + ota_last_reason());
    app::redraw(false);
  }
}

void checkJob() {
  checking = true;
  redrawAsked = true;  // System screen: "Checking…"
  checkUpdates();
  checking = false;
  checkedAtMs = millis();
  Serial.printf("[update] %s (8-bit largest %u)\n", updateText.get().c_str(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (availBuild > 0 && settings::get().autoUpdate) autoInstallReady = true;
  // Couldn't reach anything (e.g. DNS not ready just after joining Wi-Fi):
  // try again in 5 min instead of 6 h.
  if (updateText.get().indexOf("connect failed") >= 0 || updateText.get().indexOf("can't connect") >= 0)
    retrySoon = true;
  app::redraw(false);  // footer: "update ready"
}

void installJob() {
  if (availBuild <= 0) {
    checking = true;
    checkUpdates();
    checking = false;
    checkedAtMs = millis();
  }
  if (availBuild > 0) installUpdate();  // restarts on success
  else app::redraw(false);
}

// Decides when; the work runs on the worker. The GitHub check and download
// take the TLS token, so a running Hydros session steps aside on its own.
void updates() {
  if (!net::online()) return;
  if (power::quiet()) return;  // a few-second battery wake: no GitHub check
  if (worker::pending("update")) return;
  // First check as soon as Wi-Fi is up, before Hydros has opened its session,
  // then every 6 h.
  uint32_t every = retrySoon ? 5UL * 60UL * 1000UL : 6UL * 3600UL * 1000UL;
  bool due = checkAsked || !lastCheck || millis() - lastCheck > every;
  if (autoInstallReady && alarms.empty()) {
    autoInstallReady = false;
    installAsked = true;
  }
  if (installAsked) {
    installAsked = false;
    worker::post("update", installJob);
  } else if (due) {
    checkAsked = false;
    lastCheck = millis();
    retrySoon = false;
    worker::post("update", checkJob);
  }
}

}  // namespace

// ── app:: (used by the web page) ─────────────────────────────────────────
namespace app {
int page() { return curPage; }
int pages() { return nPages; }
void gotoPage(int n) {
  curPage = constrain(n, 0, max(0, nPages - 1));
  lastRotate = millis();
  infoUntil = 0;
  redrawAsked = true;
}
void nextPage() { gotoPage(nPages ? (curPage + 1) % nPages : 0); }
// A button for one page: there, and pressed again back to where you were.
void togglePage(int n) {
  static int back = 0;
  if (n < 0 || n >= nPages) return;
  if (curPage == n) gotoPage(back);
  else {
    back = curPage;
    gotoPage(n);
  }
}

// The Outlets (direct) or Schedule (Coeno) page, i.e. the first timeline page;
// pressed again, back to where you were.
void toggleSchedule() {
  static int back = 0;
  std::unique_ptr<panel::Page> p(new (std::nothrow) panel::Page());
  if (!p) return;
  for (int i = 0; i < nPages; i++) {
    if (!fetchPage(i, *p) || p->layout != panel::Layout::Timeline) continue;
    if (curPage == i) gotoPage(back);
    else {
      back = curPage;
      gotoPage(i);
    }
    return;
  }
}
void prevPage() { gotoPage(nPages ? (curPage + nPages - 1) % nPages : 0); }
void cycleView() {
  Settings &s = settings::get();
  s.view = s.view == "both" ? "graphs" : s.view == "graphs" ? "numbers" : "both";
  settings::save();
  Serial.printf("[view] %s\n", s.view.c_str());
  redrawAsked = true;
}
void redraw(bool full) {
  redrawAsked = true;
  fullAsked |= full;
}
void ackAlarm() {
  acked = true;
  board::silence();
  JsonDocument d;
  JsonArray a = d["ids"].to<JsonArray>();
  for (auto &al : alarms) a.add(al.id);
  String s;
  serializeJson(d, s);
  coeno::publishAck(s);
  redrawAsked = true;
}
String alarmsJson() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto &al : alarms) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = al.id;
    o["text"] = al.text;
  }
  String s;
  serializeJson(d, s);
  return s;
}
bool alarmAcked() { return acked; }
bool currentPage(panel::Page &p) { return fetchPage(curPage, p); }
void drawNow() {
  collectAlarms();
  fullAsked = true;
  lastDraw = millis();
  redrawAsked = false;
  draw();
}
uint32_t alarmSig() {
  uint32_t h = 2166136261u;
  for (auto &a : alarms)
    for (char ch : a.id) h = (h ^ (uint8_t)ch) * 16777619u;
  return alarms.empty() ? 0 : h;
}
void syncAlarms(bool wasAcked, uint32_t ackedSig) {
  collectAlarms();
  if (wasAcked && ackedSig && alarmSig() == ackedSig) acked = true;
  flashAsked = false;  // no flash on a battery wake (power beeps instead)
  board::silence();
}
bool updating() { return updatingFrom != nullptr; }
bool hasUnackedAlarm() { return !alarms.empty() && !acked; }
void identify() {
  identifyAsked = true;
  redrawAsked = true;
}
long availableBuild() { return availBuild; }
String updateCheckText() { return updateText.get(); }
void checkUpdateSoon() { checkAsked = true; }
void installUpdateSoon() { installAsked = true; }
void rebootSoon() {
  rebootAsked = true;
  rebootAt = millis() + 800;
}
bool rebootNeeded() { return needsReboot; }
void setRebootNeeded() { needsReboot = true; }
}  // namespace app

void setup() {
  Serial.begin(115200);
  ota_begin_boot_check();
#if defined(BOARD_RETERMINAL)
  keysBegin();
#else
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#endif
  settings::load();
  power::begin();
#if defined(BOARD_RETERMINAL)
  keysBegin();  // again: power::begin() handed the keys back from their sleep (RTC) setup
#endif
  Serial.printf("\nPlanula-ePaper build %d (%s), name %s, mode %s\n", FW_BUILD, FW_VERSION,
                settings::get().name.c_str(), settings::get().mode.c_str());
  display::begin(settings::get().panelDriver);
  if (settings::get().mode == "direct" && settings::get().hydrosOn) hydros::reserve();
  worker::begin();
  board::begin();
  Serial.printf("[mem] after start: free %u, largest block %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  net::begin();
  // The USB log is only readable with the panel in your hands (and the cable can
  // reflash it anyway), so it's the place to recover the password from.
  Serial.printf("Setup network %s, password %s (also the settings login: user admin, unless changed)\n",
                net::apName().c_str(), settings::get().apPass.c_str());
  web::begin();
  coeno::onCommand(onCoenoCommand);
  if (!power::quiet()) draw();  // a timer wake keeps the old picture until power decides
}

void loop() {
  uint32_t now = millis();
  net::loop();
  web::loop();
#if defined(BOARD_RETERMINAL)
  keys();
#else
  button();
#endif
  coeno::loop();
  board::loop();
  apex::loop();
  updates();  // before Hydros: the boot-time check runs before its session opens
  hydros::loop();
  netUpload();
  ota_tick();

  int n = max(1, countPages());
  if (n != nPages) {
    nPages = n;
    if (curPage >= nPages) curPage = 0;
    redrawAsked = true;
  }
  power::loop();  // after the page count: it may restore the page from before a sleep
  uint16_t rot = coenoMode() && coeno::rotateSeconds() ? coeno::rotateSeconds() : settings::get().rotate_s;
  if (rot && nPages > 1 && now - lastRotate > rot * 1000UL && alarms.empty()) {
    lastRotate = now;
    curPage = (curPage + 1) % nPages;
    redrawAsked = true;
  }
  if (infoUntil && now > infoUntil) {
    infoUntil = 0;
    redrawAsked = true;
  }
  if (updateFailed) {  // show what went wrong for 2 min
    updateFailed = false;
    if (coenoLinked()) infoUntil = now + 120000;
    redrawAsked = true;
  }
  // Download progress on the System screen: a partial refresh every 10 %.
  static int lastPctStep = -2;
  if (const char *from = (const char *)updatingFrom) {
    int pct = !strcmp(from, "GitHub") ? updatePct : ota_download_pct();
    int stepNow = pct < 0 ? -1 : pct / 10;
    if (stepNow != lastPctStep) {
      lastPctStep = stepNow;
      redrawAsked = true;
    }
  } else {
    lastPctStep = -2;
  }

  // Redraw on request, or once a minute (clock + fresh values). The frame hash
  // in display::show() skips the refresh when nothing visible changed.
  static int lastMinute = -1;
  int minute = net::timeValid() ? (int)((time(nullptr) / 60) % 1440) : (int)(now / 60000);
  if (minute != lastMinute) {
    lastMinute = minute;
    redrawAsked = true;
  }
  static uint32_t seenKeys = 0;
  if (readings::newKeys() != seenKeys) {  // a gauge may have just got its first value
    seenKeys = readings::newKeys();
    redrawAsked = true;
  }
  if (power::quiet()) redrawAsked = false;  // power draws at most once per timer wake
  if (redrawAsked && now - lastDraw > 400) {
    redrawAsked = false;
    lastDraw = now;
    collectAlarms();
    draw();
  }
  // Alarm reminder: one short flash every 5 min until acknowledged.
  if (!alarms.empty() && !acked && !power::sleepy() && now - lastReminder > 300000UL) {
    lastReminder = now;
    display::flash(1);
    board::alarmBeep();
  }
  stateOnChange();
  static uint32_t lastMemLog = 0;
  if (now - lastMemLog > 60000UL) {
    lastMemLog = now;
    // 8-bit-capable RAM is what TLS, Wi-Fi and the page can use (getFreeHeap
    // also counts IRAM that only takes 32-bit words).
    Serial.printf("[mem] free %u (8-bit %u), largest 8-bit block %u, lowest %u; stack unused: loop %u, net %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)worker::stackFree());
  }
  if (now - lastState > 60000UL) {  // heartbeat
    lastState = now;
    publishState();
  }
  if (rebootAsked && (int32_t)(now - rebootAt) > 0) ESP.restart();
  delay(5);
}
