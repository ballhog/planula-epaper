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

#include <vector>

#include "Apex.h"
#include "App.h"
#include "CoenoLink.h"
#include "DirectPages.h"
#include "Display.h"
#include "Https.h"
#include "Hydros.h"
#include "Net.h"
#include "Render.h"
#include "Settings.h"
#include "Web.h"
#include "ota.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const int PIN_BUTTON = 0;  // BOOT

// Pages (a few kB each) and the renderer live on the loop task's stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {

int curPage = 0, nPages = 1;
uint32_t lastRotate = 0, lastDraw = 0, lastState = 0, infoUntil = 0, lastReminder = 0;
bool redrawAsked = true, fullAsked = false, identifyAsked = false;
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
long availBuild = 0;
String updateText = "not checked yet";
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
  if (!coenoMode()) {
    direct::build(i, p);
    return true;
  }
  uint32_t at;
  if (!coeno::getPage(i, p, at)) return false;
  uint32_t pageAge = (millis() - at) / 1000;
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
  panel::Page p;
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
    alert = (s.apexOn && !apex::ok() && millis() - apex::lastOkMs() > s.stale_s * 1000UL) ||
            (s.hydrosOn && !hydros::ok() && millis() > 180000UL);
  }
  if (net::online()) right = "Wi-Fi " + String(net::rssi()) + " dBm";
  else right = "Wi-Fi offline";
  if (availBuild > 0) right += " · update ready";
  PANEL_COPY(c.footerLeft, left.c_str());
  PANEL_COPY(c.footerRight, right.c_str());
  c.footerAlert = alert;
}

void drawInfo(panel::Chrome &c) {
  const Settings &s = settings::get();
  String body = "Settings:  http://" + s.name + ".local  or  http://" + net::ip() + "\n";
  body += "Mode: " + s.mode + "   ·   " + net::statusText() + "\n";
  body += "Coeno: " + coeno::status() + "\n";
  if (s.apexOn) body += "Apex: " + apex::status() + "\n";
  if (s.hydrosOn) body += "Hydros: " + hydros::status() + "\n";
  body += "Build " + String(FW_BUILD) + " (" FW_VERSION ")";
  panel::renderMessage(display::canvas(), s.name.c_str(), body.c_str(), net::ip().c_str(), c);
}

void draw() {
  panel::Chrome c;
  fillChrome(c);
  GFXcanvas1 &cv = display::canvas();
  const Settings &s = settings::get();

  if (net::state() == net::State::SetupAp && !net::online()) {
    panel::SetupInfo si;
    String ap = net::apName(), url = "http://192.168.4.1", why = net::setupReason();
    si.apName = ap.c_str();
    si.apPass = s.apPass.c_str();
    si.url = url.c_str();
    si.reason = why.c_str();
    String left = s.name + " · setup mode";
    PANEL_COPY(c.footerLeft, left.c_str());
    c.footerAlert = false;
    snprintf(c.footerRight, sizeof c.footerRight, "build %d", (int)FW_BUILD);
    panel::renderSetup(cv, si, c);
  } else if (millis() < infoUntil) {
    drawInfo(c);
  } else if (!net::online()) {
    panel::renderMessage(cv, "Joining Wi-Fi", ("Looking for " + (s.wifi.empty() ? String("a network") : s.wifi[0].ssid) +
                                                 " …\nHold the BOOT button 10 s to open the setup network.").c_str(),
                         "", c);
  } else {
    panel::Page p;
    if (!fetchPage(curPage, p)) {
      String body;
      const char *title;
      if (coenoMode()) {
        title = "Waiting for Coeno";
        body = "Connected to Wi-Fi as " + net::ip() + ".\nCoeno: " + coeno::status() +
               ".\nAdd this panel in Coeno, or switch to direct mode at\nhttp://" + s.name + ".local";
      } else {
        title = "No gauges yet";
        body = "Pick what to show at http://" + s.name + ".local\n(or http://" + net::ip() + ")";
      }
      panel::renderMessage(cv, title, body.c_str(), net::ip().c_str(), c);
    } else {
      panel::render(cv, p, c);
    }
  }
  // A new build has proven itself once it is on Wi-Fi and drawing.
  if (!healthy && net::online()) {
    ota_mark_healthy();
    healthy = true;
  }
  bool full = fullAsked;
  fullAsked = false;
  display::show(full);
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
      panel::renderMessage(display::canvas(), "Settings erased", "Restarting into setup mode…", "", c);
      display::show(true);
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
      infoUntil = now + 60000;
      app::redraw(false);
    } else if (held >= 40) {
      if (!alarms.empty() && !acked) app::ackAlarm();
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
      panel::renderMessage(display::canvas(), "Updating firmware", "Receiving from the Mac. Don't unplug the panel.", "", c);
      display::show(false);
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
const char *kGithubManifest = "https://github.com/ballhog/planula-epaper/releases/latest/download/latest.json";
String availSource, availUrl, availSha;
size_t availSize = 0;

void showUpdating(const char *from) {
  panel::Chrome c;
  fillChrome(c);
  String body = String("Downloading from ") + from + ". Don't unplug the panel.\nIt restarts by itself when done.";
  panel::renderMessage(display::canvas(), "Updating firmware", body.c_str(), "", c);
  display::show(false);
}

void checkUpdates() {
  const Settings &s = settings::get();
  availBuild = 0;
  availSource = "";
  String coenoWhy;
  if (s.coenoUpdates && s.coenoHost.length()) {
    String url = "http://" + s.coenoHost + ":8000/firmware/planula-epaper/latest.json";
    long b = ota_check(url.c_str());
    if (b > 0) {
      availBuild = b;
      availSource = "coeno";
      updateText = "build " + String(b) + " ready on Coeno";
      return;
    }
    if (b == 0 && ota_last_http() != 404) {
      updateText = "up to date (build " + String(FW_BUILD) + ", Coeno)";
      return;
    }
    coenoWhy = String("Coeno: ") + ota_last_reason() + "; ";
  }
  if (!s.githubUpdates) {
    updateText = coenoWhy.length() ? coenoWhy + "GitHub updates off" : "update checks off";
    return;
  }
  JsonDocument m;
  https::Result r = https::requestJson("GET", kGithubManifest, {{"Accept", "application/json"}}, "", m, nullptr);
  if (r.status != 200 || r.error.length()) {
    updateText = coenoWhy + "GitHub: " + (r.status < 0 ? r.error : "HTTP " + String(r.status));
    return;
  }
  long b = m["build"] | 0L;
  if (b > FW_BUILD && m["url"].is<const char *>()) {
    availBuild = b;
    availSource = "github";
    availUrl = m["url"].as<const char *>();
    availSha = m["sha256"] | "";
    availSize = m["size"] | 0;
    updateText = "build " + String(b) + " ready on GitHub";
  } else {
    updateText = coenoWhy + "up to date (build " + String(FW_BUILD) + ", GitHub)";
  }
}

void installUpdate() {
  if (availSource == "github") {
    showUpdating("GitHub");
    String e = https::downloadFirmware(availUrl, availSha, availSize, nullptr);
    if (e.length()) {
      updateText = "update failed: " + e;
      app::redraw(false);
      return;
    }
    ota_mark_pending();
    Serial.printf("[update] build %ld from GitHub installed, restarting\n", availBuild);
    delay(300);
    ESP.restart();
  }
  const Settings &s = settings::get();
  String url = "http://" + s.coenoHost + ":8000/firmware/planula-epaper/latest.json";
  showUpdating("Coeno");
  if (!ota_check_and_apply(url.c_str(), nullptr)) {
    updateText = String("update failed: ") + ota_last_reason();
    app::redraw(false);
  }
}

void updates() {
  const Settings &s = settings::get();
  if (!net::online()) return;
  bool due = checkAsked || (millis() > 90000 && (!lastCheck || millis() - lastCheck > 6UL * 3600UL * 1000UL));
  if (due) {
    checkAsked = false;
    lastCheck = millis();
    checkUpdates();
    Serial.printf("[update] %s\n", updateText.c_str());
    if (availBuild > 0 && s.autoUpdate && alarms.empty()) installAsked = true;
  }
  if (installAsked) {
    installAsked = false;
    if (availBuild <= 0) checkUpdates();
    if (availBuild > 0) installUpdate();
    else app::redraw(false);
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
String updateCheckText() { return updateText; }
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
  hydros::loop();
  updates();
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

  // Redraw on request, or once a minute (clock + fresh values). The frame hash
  // in display::show() skips the refresh when nothing visible changed.
  static int lastMinute = -1;
  int minute = net::timeValid() ? (int)((time(nullptr) / 60) % 1440) : (int)(now / 60000);
  if (minute != lastMinute) {
    lastMinute = minute;
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
    Serial.printf("[mem] free %u, largest block %u, lowest %u\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
    publishState();
  }
  if (rebootAsked && (int32_t)(now - rebootAt) > 0) ESP.restart();
  delay(5);
}
