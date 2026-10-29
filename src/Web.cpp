#include "Web.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "Apex.h"
#include "App.h"
#include "CoenoLink.h"
#include "Display.h"
#include "Hydros.h"
#include "Net.h"
#include "Readings.h"
#include "Settings.h"
#include "web_ui.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace web {

namespace {

WebServer server(80);

bool allowed() {
  const String &p = settings::get().adminPass;
  if (!p.length()) return true;
  if (server.authenticate("admin", p.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Planula panel", "Password needed");
  return false;
}

void sendJson(int code, const JsonDocument &d) {
  String s;
  serializeJson(d, s);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", s);
}

void sendRaw(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

bool body(JsonDocument &d) {
  if (!server.hasArg("plain")) return true;
  return !deserializeJson(d, server.arg("plain"));
}

void ok(const char *msg = "ok") {
  JsonDocument d;
  d["ok"] = true;
  d["message"] = msg;
  sendJson(200, d);
}

void err(int code, const String &msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  sendJson(code, d);
}

// Captive portal: phones on the setup network get sent to the page.
bool captive() {
  if (net::state() != net::State::SetupAp) return false;
  String host = server.hostHeader();
  String apIp = WiFi.softAPIP().toString();
  if (host == apIp || host.endsWith(".local") || host == net::ip()) return false;
  server.sendHeader("Location", "http://" + apIp + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

void handleRoot() {
  if (captive()) return;
  if (!allowed()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", WEB_UI);
}

void handleStatus() {
  if (!allowed()) return;
  const Settings &s = settings::get();
  JsonDocument d;
  d["name"] = s.name;
  d["build"] = FW_BUILD;
  d["version"] = FW_VERSION;
  d["board"] = NODE_BOARD;
  d["mode"] = s.mode;
  d["uptime_s"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["heap_min"] = ESP.getMinFreeHeap();
  JsonObject w = d["wifi"].to<JsonObject>();
  w["online"] = net::online();
  w["ssid"] = net::ssid();
  w["rssi"] = net::rssi();
  w["ip"] = net::ip();
  w["text"] = net::statusText();
  w["setup_open"] = net::state() == net::State::SetupAp;
  w["ap_name"] = net::apName();
  JsonObject c = d["coeno"].to<JsonObject>();
  c["connected"] = coeno::connected();
  c["text"] = coeno::status();
  c["pages"] = coeno::pageCount();
  JsonObject a = d["apex"].to<JsonObject>();
  a["ok"] = apex::ok();
  a["text"] = apex::status();
  JsonObject h = d["hydros"].to<JsonObject>();
  h["ok"] = hydros::ok();
  h["text"] = hydros::status();
  d["page"] = app::page();
  d["pages"] = app::pages();
  d["alarms"] = serialized(app::alarmsJson());
  d["acked"] = app::alarmAcked();
  JsonObject u = d["update"].to<JsonObject>();
  u["available_build"] = app::availableBuild();
  u["text"] = app::updateCheckText();
  d["reboot_needed"] = app::rebootNeeded();
  d["refreshes"] = display::refreshes();
  d["full_refreshes"] = display::fullRefreshes();
  sendJson(200, d);
}

void handleGetSettings() {
  if (!allowed()) return;
  JsonDocument d;
  settings::toJson(d.to<JsonObject>(), false);
  sendJson(200, d);
}

void handlePostSettings() {
  if (!allowed()) return;
  JsonDocument in;
  if (!body(in)) return err(400, "bad JSON");
  Settings before = settings::get();
  String e = settings::applyJson(in.as<JsonObjectConst>());
  if (e.length()) return err(400, e);
  if (!settings::save()) return err(500, "couldn't save");
  const Settings &s = settings::get();
  bool wifiChanged = before.wifi.size() != s.wifi.size();
  for (size_t i = 0; !wifiChanged && i < s.wifi.size(); i++)
    wifiChanged = before.wifi[i].ssid != s.wifi[i].ssid || before.wifi[i].pass != s.wifi[i].pass;
  if (wifiChanged) net::reconnect();
  if (before.coenoHost != s.coenoHost || before.coenoPort != s.coenoPort || before.name != s.name) coeno::restart();
  if (before.name != s.name) app::setRebootNeeded();  // mDNS name
  if (before.panelDriver != s.panelDriver) app::setRebootNeeded();
  if (before.hydrosUser != s.hydrosUser || before.hydrosPass != s.hydrosPass || before.hydrosOn != s.hydrosOn ||
      before.mode != s.mode) {
    hydros::restart();
    if (!s.hydrosOn) readings::clearSource("Hydros");
  }
  if (before.apexHost != s.apexHost || before.apexUser != s.apexUser || before.apexPass != s.apexPass ||
      before.apexOn != s.apexOn || before.mode != s.mode) {
    apex::pollNow();
    if (!s.apexOn) readings::clearSource("Apex");
  }
  app::redraw(before.mode != s.mode);
  JsonDocument d;
  d["ok"] = true;
  d["reboot_needed"] = app::rebootNeeded();
  sendJson(200, d);
}

void handleScan() {
  if (!allowed()) return;
  sendRaw(200, net::scanJson());
}

void handleTestApex() {
  if (!allowed()) return;
  JsonDocument in;
  body(in);
  const Settings &s = settings::get();
  String host = in["host"] | s.apexHost.c_str();
  String user = in["user"] | s.apexUser.c_str();
  String pass = in["pass"] | "";
  if (!pass.length()) pass = s.apexPass;
  JsonDocument d;
  d["result"] = apex::test(host, user, pass);
  sendJson(200, d);
}

void handleTestHydros() {
  if (!allowed()) return;
  JsonDocument in;
  body(in);
  const Settings &s = settings::get();
  String user = in["user"] | s.hydrosUser.c_str();
  String pass = in["pass"] | "";
  if (!pass.length()) pass = s.hydrosPass;
  JsonDocument d;
  d["result"] = hydros::test(user, pass);
  sendJson(200, d);
}

void handleReadings() {
  if (!allowed()) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto &r : readings::all()) {
    JsonObject o = a.add<JsonObject>();
    o["key"] = r.key;
    o["source"] = r.source;
    o["device"] = r.device;
    o["name"] = r.name;
    o["kind"] = r.kind;
    o["unit"] = r.unit;
    if (isnan(r.value)) o["value"] = nullptr;
    else o["value"] = r.value;
    o["age_s"] = (millis() - r.atMs) / 1000;
  }
  sendJson(200, d);
}

void handlePage() {
  if (!allowed()) return;
  JsonDocument in;
  body(in);
  if (in["next"] | false) app::nextPage();
  else app::gotoPage(in["n"] | 0);
  ok();
}

void handleRefresh() {
  if (!allowed()) return;
  JsonDocument in;
  body(in);
  app::redraw(in["full"] | false);
  ok();
}

void handleScreen() {
  if (!allowed()) return;
  size_t n = display::bmpSize();
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) return err(503, "out of memory");
  display::writeBmp(buf);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(n);
  server.send(200, "image/bmp", "");
  NetworkClient cl = server.client();
  size_t off = 0;
  uint32_t start = millis();
  while (off < n && cl.connected() && millis() - start < 15000) {
    size_t w = cl.write(buf + off, min<size_t>(1024, n - off));
    if (w == 0) {  // send buffer full on a weak link: wait, don't drop
      delay(20);
      continue;
    }
    off += w;
  }
  free(buf);
}

void handleFactory() {
  if (!allowed()) return;
  JsonDocument in;
  body(in);
  if (String(in["confirm"] | "") != "erase") return err(400, "send {\"confirm\":\"erase\"}");
  settings::factoryReset();
  ok("erased; restarting");
  app::rebootSoon();
}

void handleNotFound() {
  if (captive()) return;
  server.send(404, "text/plain", "not found");
}

}  // namespace

void begin() {
  static const char *headers[] = {"Authorization"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.on("/api/wifi/scan", HTTP_GET, handleScan);
  server.on("/api/test/apex", HTTP_POST, handleTestApex);
  server.on("/api/test/hydros", HTTP_POST, handleTestHydros);
  server.on("/api/readings", HTTP_GET, handleReadings);
  server.on("/api/page", HTTP_POST, handlePage);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/ack", HTTP_POST, [] { if (allowed()) { app::ackAlarm(); ok(); } });
  server.on("/api/identify", HTTP_POST, [] { if (allowed()) { app::identify(); ok(); } });
  server.on("/api/update/check", HTTP_POST, [] { if (allowed()) { app::checkUpdateSoon(); ok("checking"); } });
  server.on("/api/update/install", HTTP_POST, [] { if (allowed()) { app::installUpdateSoon(); ok("installing"); } });
  server.on("/api/reboot", HTTP_POST, [] { if (allowed()) { ok("restarting"); app::rebootSoon(); } });
  server.on("/api/factory-reset", HTTP_POST, handleFactory);
  server.on("/screen.bmp", HTTP_GET, handleScreen);
  // Phones probe these to detect a captive portal.
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/connecttest.txt", HTTP_GET, handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() { server.handleClient(); }

}  // namespace web
