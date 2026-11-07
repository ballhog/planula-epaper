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
#include "Shared.h"
#include "Log.h"

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

namespace ui {


int curPage = 0, nPages = 1;
uint32_t lastRotate = 0, lastDraw = 0, lastState = 0, infoUntil = 0, lastReminder = 0;
volatile bool redrawAsked = true;  // also set from the worker
bool fullAsked = false, identifyAsked = false;
bool healthy = false;

// Alarms across all pages.
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

String lastScreen;

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

}  // namespace ui

using namespace ui;


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
  plog.printf("[view] %s\n", s.view.c_str());
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
  plog.printf("\nPlanula-ePaper build %d (%s), name %s, mode %s\n", FW_BUILD, FW_VERSION,
                settings::get().name.c_str(), settings::get().mode.c_str());
  display::begin(settings::get().panelDriver);
  if (settings::get().mode == "direct" && settings::get().hydrosOn) hydros::reserve();
  worker::begin();
  board::begin();
  plog.printf("[mem] after start: free %u, largest block %u\n", (unsigned)ESP.getFreeHeap(),
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
    plog.printf("[mem] free %u (8-bit %u), largest 8-bit block %u, lowest %u; stack unused: loop %u, net %u\n",
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
