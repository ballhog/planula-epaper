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
SET_LOOP_TASK_STACK_SIZE(8 * 1024);

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

int countPages() { return coenoMode() ? coeno::pageCount() : direct::pageCount(); }

// Fill page i with fresh data (Coeno: received page + local staleness).
bool fetchPage(int i, panel::Page &p) {
  p.h12 = !settings::get().clock24;
  if (!coenoMode()) {
    direct::build(i, p);
    return true;
  }
  uint32_t at;
  if (!coeno::getPage(i, p, at)) return false;
  p.h12 = !settings::get().clock24;
  uint32_t pageAge = (millis() - at) / 1000;
  panel::shiftTimeline(p, pageAge / 60);  // "now" moves on between Coeno's pages
  uint16_t staleAfter = settings::get().stale_s;
  for (int k = 0; k < p.n; k++) {
    panel::Gauge &g = p.g[k];
    g.age_s += pageAge;
    if (g.has && g.age_s > staleAfter && g.status != panel::Status::Alarm) g.status = panel::Status::Stale;
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
  }
  if (now.empty()) acked = false;
  alarms = now;
}

void fillChrome(panel::Chrome &c) {
  const Settings &s = settings::get();
  PANEL_COPY(c.clock, clockText().c_str());
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
    if (!coeno::connected()) {
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
  if (net::online()) right = "Wi-Fi " + String(net::rssi()) + " dBm";
  else right = "Wi-Fi offline";
  if (availBuild > 0) right += " · update ready, hold 3 s";
  PANEL_COPY(c.footerLeft, left.c_str());
  PANEL_COPY(c.footerRight, right.c_str());
  c.footerAlert = alert;
}

// A message screen as a scene that owns its text.
display::Scene message(const String &title, const String &body, const String &big, const panel::Chrome &c) {
  return [title, body, big, c](Adafruit_GFX &d) { panel::renderMessage(d, title.c_str(), body.c_str(), big.c_str(), c); };
}

// System screen (button held 3 s, an install running, or one that just failed):
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

  if (updatingFrom) {
    scene = drawSystem(c);
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
    scene = drawSystem(c);
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
      if (coenoMode()) {
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
      screen = "page:" + String(curPage) + ":" + String(p->n) + ":" + String(nPages);
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
  String s;
  serializeJson(d, s);
  coeno::publishState(s);
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
  else if (leaf == "ack") { acked = true; app::redraw(false); }
}

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
      if (infoUntil && now < infoUntil) app::checkUpdateSoon();
      infoUntil = now + 120000;
      app::redraw(false);
    } else if (held >= 40) {
      if (infoUntil && now < infoUntil) {
        // System screen: press = install when there's one (or retry), else close.
        if (availBuild > 0) {
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
  if (b > FW_BUILD && m["url"].is<const char *>()) {
    availBuild = b;
    availSource = "github";
    availUrl = m["url"].as<const char *>();
    availSha = m["sha256"] | "";
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
  if (worker::pending("update")) return;
  // First check as soon as Wi-Fi is up, before Hydros has opened its session,
  // then every 6 h.
  bool due = checkAsked || !lastCheck || millis() - lastCheck > 6UL * 3600UL * 1000UL;
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
void redraw(bool full) {
  redrawAsked = true;
  fullAsked |= full;
}
void ackAlarm() {
  acked = true;
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
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  settings::load();
  Serial.printf("\nPlanula-ePaper build %d (%s), name %s, mode %s\n", FW_BUILD, FW_VERSION,
                settings::get().name.c_str(), settings::get().mode.c_str());
  display::begin(settings::get().panelDriver);
  if (settings::get().mode == "direct" && settings::get().hydrosOn) hydros::reserve();
  worker::begin();
  Serial.printf("[mem] after start: free %u, largest block %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  net::begin();
  web::begin();
  coeno::onCommand(onCoenoCommand);
  draw();
}

void loop() {
  uint32_t now = millis();
  net::loop();
  web::loop();
  button();
  coeno::loop();
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
    infoUntil = now + 120000;
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
  if (redrawAsked && now - lastDraw > 400) {
    redrawAsked = false;
    lastDraw = now;
    collectAlarms();
    draw();
  }
  // Alarm reminder: one short flash every 5 min until acknowledged.
  if (!alarms.empty() && !acked && now - lastReminder > 300000UL) {
    lastReminder = now;
    display::flash(1);
  }
  if (now - lastState > 60000UL) {
    lastState = now;
    // 8-bit-capable RAM is what TLS, Wi-Fi and the page can use (getFreeHeap
    // also counts IRAM that only takes 32-bit words).
    Serial.printf("[mem] free %u (8-bit %u), largest 8-bit block %u, lowest %u; stack unused: loop %u, net %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)worker::stackFree());
    publishState();
  }
  if (rebootAsked && (int32_t)(now - rebootAt) > 0) ESP.restart();
  delay(5);
}
