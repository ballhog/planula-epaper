#include "Net.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

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

String chooseSsid() {
  const Settings &s = settings::get();
  if (s.wifi.empty()) return "";
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
  } else {
    attempt();
  }
}

void loop() {
  if (apOn) dns.processNextRequest();
  const Settings &s = settings::get();
  bool up = WiFi.isConnected();
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
bool online() { return WiFi.isConnected(); }
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
