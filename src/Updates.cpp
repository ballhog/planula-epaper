// Firmware updates: Coeno / GitHub check and install, upload from the Mac.
// Split out of main.cpp; shared state in Shared.h.
#include "Shared.h"

namespace ui {

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

}  // namespace ui
