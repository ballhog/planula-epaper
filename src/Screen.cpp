// Screen: chrome (footer), building pages, drawing.
// Split out of main.cpp; shared state in Shared.h.
#include "Shared.h"

namespace ui {

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
  // Built in fixed buffers: this runs on every draw, and on the Waveshare
  // board short-lived Strings fragment the heap over days.
  char left[96] = "", right[48] = "";
  bool alert = false;
  if (coenoMode()) {
    uint32_t lp = coeno::lastPageMs();
    uint32_t age = lp ? (millis() - lp) / 1000 : 0;
    if (!s.coenoHost.length()) {
      snprintf(left, sizeof left, "Standalone · no Coeno address set");
    } else if (!coeno::connected()) {
      snprintf(left, sizeof left, "Coeno offline · showing the last pages");
      alert = true;
    } else if (lp && age > s.stale_s) {
      char a[16];
      panel::formatAge(age, a, sizeof a);
      snprintf(left, sizeof left, "STALE · no page from Coeno for %s", a);
      alert = true;
    } else {
      String ck = clockText();
      snprintf(left, sizeof left, "Coeno · %s%s%s", s.coenoHost.c_str(), ck.length() ? " · updated " : "", ck.c_str());
    }
  } else {
    size_t n = 0;
    if (s.apexOn) n += snprintf(left + n, sizeof left - n, "Apex %s", apex::ok() ? "ok" : "offline");
    if (s.hydrosOn && n < sizeof left)
      n += snprintf(left + n, sizeof left - n, "%sHydros %s", n ? " · " : "", hydros::ok() ? "ok" : "offline");
    if (!n) snprintf(left, sizeof left, "Direct mode: no source switched on");
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
  // Room sensor (reTerminal) in the footer: "72°F  45%".
  float rc = board::roomC(), rh = board::roomRh();
  if (!isnan(rc)) {
    bool f = s.tempF;
    snprintf(c.room, sizeof c.room, "%.0f°%s  %.0f%%", f ? rc * 9 / 5 + 32 : rc, f ? "F" : "C", rh);
  }
  if (power::sleepy()) {
    // The screen is a snapshot: say when it was taken and when the next one is.
    String at = clockText(), nx = power::nextWakeText();
    char status[96];
    strlcpy(status, left, sizeof status);
    snprintf(left, sizeof left, "Updated%s%s%s%s · %s", at.length() ? " " : "", at.c_str(),
             nx.length() ? " · next " : "", nx.c_str(), status);
  }
  if (availBuild > 0 && coenoLinked()) snprintf(right, sizeof right, "update ready, hold 3 s");
  PANEL_COPY(c.footerLeft, left);
  PANEL_COPY(c.footerRight, right);
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

}  // namespace ui
