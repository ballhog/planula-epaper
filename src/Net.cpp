#include "Net.h"
#include "Log.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

#include <type_traits>

#include "Settings.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace net {

namespace {

State st = State::Connecting;
DNSServer dns;
bool apOn = false, mdnsOn = false, ntpOn = false;
uint32_t tryStart = 0, lastAttempt = 0, bootMs = 0, apSince = 0;
String reason, apSsid, joinedSsid, lastTz;
bool wantReconnect = true;

// The access point last joined, kept through deep sleep: after a battery wake
// the panel goes straight to it (no 2-3 s scan). Normal join if that fails.
struct FastJoin {
  uint32_t magic;
  char ssid[33];
  uint8_t bssid[6];
  int32_t channel;
};
constexpr uint32_t kFastMagic = 0x46534A31;  // "FSJ1"
RTC_DATA_ATTR FastJoin fast;
static_assert(std::is_trivially_default_constructible<FastJoin>::value, "FastJoin must be plain data (RTC memory)");
bool fastTry = false;
uint32_t fastAt = 0;

String chooseSsid() {
  const Settings &s = settings::get();
  if (s.wifi.empty()) return "";
  // One saved network: nothing to choose, skip the scan (~2 s at every boot).
  if (s.wifi.size() == 1) return s.wifi[0].ssid;
  int n = WiFi.scanNetworks(false, false);
  String best;
  int bestRssi = -1000;
  for (int i = 0; i < n; i++)
    for (auto &w : s.wifi)
      if (WiFi.SSID(i) == w.ssid && WiFi.RSSI(i) > bestRssi) {
        best = w.ssid;
        bestRssi = WiFi.RSSI(i);
      }
  if (!best.length()) best = s.wifi[0].ssid;  // hidden network: try the first saved one
  return best;
}

void startAp() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), settings::get().apPass.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  apSince = millis();
  st = State::SetupAp;
}

void stopAp() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
}

void startServices() {
  const Settings &s = settings::get();
  plog.printf("[wifi] joined %s, IP %s, gateway %s, DNS %s, %d dBm, %u ms after boot%s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP(0).toString().c_str(), WiFi.RSSI(), (unsigned)millis(), fastTry ? " (fast rejoin)" : "");
  fastTry = false;
  // Remember the access point for the next wake.
  if (uint8_t *b = WiFi.BSSID()) {
    fast.magic = kFastMagic;
    strlcpy(fast.ssid, WiFi.SSID().c_str(), sizeof fast.ssid);
    memcpy(fast.bssid, b, 6);
    fast.channel = WiFi.channel();
  }
  if (!mdnsOn && MDNS.begin(s.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("calyx", "tcp", 80);
    MDNS.addServiceTxt("calyx", "tcp", "type", "panel");
    MDNS.addServiceTxt("calyx", "tcp", "build", String(FW_BUILD));
    mdnsOn = true;
  }
  if (!ntpOn || lastTz != s.tz) {
    configTzTime(s.tz.c_str(), "pool.ntp.org", "time.google.com", s.coenoHost.length() ? s.coenoHost.c_str() : nullptr);
    ntpOn = true;
    lastTz = s.tz;
  }
}

void attempt() {
  const Settings &s = settings::get();
  String ss = chooseSsid();
  if (!ss.length()) return;
  for (auto &w : s.wifi)
    if (w.ssid == ss) {
      WiFi.setHostname(s.name.c_str());
      WiFi.begin(w.ssid.c_str(), w.pass.c_str());
      joinedSsid = ss;
      tryStart = millis();
      return;
    }
}

}  // namespace

void begin() {
  bootMs = millis();
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char b[24];
  snprintf(b, sizeof b, "Planula-%02X%02X", mac[4], mac[5]);
  apSsid = b;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings::get().name.c_str());
  if (settings::get().wifi.empty()) {
    reason = "No Wi-Fi saved yet.";
    startAp();
  } else if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED && fast.magic == kFastMagic) {
    // Woken from deep sleep: straight to the access point from last time.
    for (auto &w : settings::get().wifi)
      if (w.ssid == fast.ssid) {
        WiFi.begin(w.ssid.c_str(), w.pass.c_str(), fast.channel, fast.bssid, true);
        joinedSsid = w.ssid;
        tryStart = lastAttempt = fastAt = millis();
        fastTry = true;
        wantReconnect = false;
        return;
      }
    attempt();
  } else {
    attempt();
  }
}

// Joined AND has an address. WiFi.isConnected() turns true when the access
// point accepts us, before DHCP has given an IP: the reTerminal's first boots
// started services and the update check at "IP 0.0.0.0, DNS 0.0.0.0", so every
// lookup failed.
static bool hasIp() { return WiFi.isConnected() && (uint32_t)WiFi.localIP() != 0; }

void loop() {
  if (apOn) dns.processNextRequest();
  const Settings &s = settings::get();
  bool up = hasIp();
  if (up) {
    if (st != State::Online) {
      st = State::Online;
      startServices();
    }
    // Leave the setup network up for 2 min after joining, so the phone that
    // did the setup gets the "connected" answer, then close it.
    if (apOn && millis() - apSince > 120000) stopAp();
    if (lastTz != s.tz) startServices();
    wantReconnect = false;
    return;
  }
  if (st == State::Online) {
    st = State::Connecting;
    tryStart = millis();
  }
  if (s.wifi.empty()) {
    if (!apOn) {
      reason = "No Wi-Fi saved yet.";
      startAp();
    }
    return;
  }
  if (fastTry && millis() - fastAt > 6000) {  // the remembered access point didn't answer
    plog.println("[wifi] fast rejoin failed, scanning");
    fastTry = false;
    fast.magic = 0;
    WiFi.disconnect(false, false);
    wantReconnect = true;
  }
  if (fastTry) return;
  if (wantReconnect || millis() - lastAttempt > 30000) {
    wantReconnect = false;
    lastAttempt = millis();
    attempt();
  }
  // Not joined for 60 s (at boot or after a drop): open the setup network too.
  if (!apOn && millis() - tryStart > 60000) {
    reason = "Can't join " + (joinedSsid.length() ? joinedSsid : String("saved Wi-Fi")) + ".";
    startAp();
  }
}

State state() { return st; }
bool online() { return hasIp(); }
String ip() { return WiFi.isConnected() ? WiFi.localIP().toString() : (apOn ? WiFi.softAPIP().toString() : ""); }
String apName() { return apSsid; }
String ssid() { return WiFi.isConnected() ? WiFi.SSID() : ""; }
int rssi() { return WiFi.isConnected() ? WiFi.RSSI() : 0; }
String setupReason() { return reason; }

String statusText() {
  if (WiFi.isConnected()) return "Wi-Fi " + WiFi.SSID() + " · " + String(WiFi.RSSI()) + " dBm · " + WiFi.localIP().toString();
  if (apOn) return "setup network " + apSsid + " open · " + reason;
  return "joining " + joinedSsid;
}

void reconnect() {
  WiFi.disconnect(false, false);
  wantReconnect = true;
  tryStart = millis();
}

void openSetup(const char *why) {
  reason = why;
  startAp();
}

String scanJson() {
  int n = WiFi.scanNetworks(false, true);
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n && i < 30; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;
    for (JsonObject o : a)
      if (ss == (const char *)o["ssid"]) {
        dup = true;
        if (WiFi.RSSI(i) > (int)o["rssi"]) o["rssi"] = WiFi.RSSI(i);
      }
    if (dup) continue;
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = ss;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String s;
  serializeJson(d, s);
  return s;
}

bool timeValid() { return time(nullptr) > 1700000000; }

}  // namespace net
