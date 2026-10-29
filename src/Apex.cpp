#include "Apex.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "Readings.h"
#include "Worker.h"
#include "Settings.h"

namespace apex {

// loop() decides when; the poll itself runs on the worker task.
static uint32_t lastPoll = 0;
static volatile uint32_t lastOk = 0;
static volatile bool force = true, okNow = false;
static SharedText state("not set up");
static volatile int inputs = 0;
static int8_t shown = -1;  // idle text on screen (-1 none): don't rewrite it every loop

static float parseValue(JsonVariantConst v) {
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    float f = strtof(s, &end);
    if (end != s) return f;
  }
  return NAN;
}

static bool reportsF(JsonVariantConst raw, float v, const String &unitSetting) {
  if (unitSetting == "F") return true;
  if (unitSetting == "C") return false;
  if (raw.is<const char *>()) {
    String s = raw.as<const char *>();
    s.trim();
    if (s.endsWith("F")) return true;
    if (s.endsWith("C")) return false;
  }
  return v > 40.0f;  // no reef runs above 40 °C, so this must be °F
}

// Returns "" on success, else an error. publish = write to readings.
static String poll(const String &hostIn, const String &user, const String &pass, bool publish, int *count) {
  if (!WiFi.isConnected()) return "no Wi-Fi";
  String host = hostIn;
  host.trim();
  if (!host.length()) return "no Apex address set";
  if (!host.startsWith("http")) host = "http://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);

  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(5000);
  if (!http.begin(host + "/rest/status")) return "bad address";
  http.setAuthorization(user.c_str(), pass.c_str());
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code == 401 || code == 403) {
    http.end();
    return "login refused (check user/password)";
  }
  if (code != 200) {
    http.end();
    return code < 0 ? String("can't reach Apex (") + http.errorToString(code) + ")" : "HTTP " + String(code);
  }

  // Keep only the inputs: the full status (outputs, feed, power) can be 30 kB+.
  JsonDocument filter;
  JsonObject in = filter["inputs"][0].to<JsonObject>();
  in["name"] = true;
  in["type"] = true;
  in["value"] = true;
  filter["istat"]["inputs"][0] = in;
  filter["status"]["inputs"][0] = in;
  filter["system"]["hostname"] = true;
  filter["hostname"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return String("bad reply: ") + err.c_str();

  JsonArrayConst arr = doc["inputs"].as<JsonArrayConst>();
  if (arr.isNull()) arr = doc["istat"]["inputs"].as<JsonArrayConst>();
  if (arr.isNull()) arr = doc["status"]["inputs"].as<JsonArrayConst>();
  if (arr.isNull()) return "no inputs in reply";
  String dev = doc["system"]["hostname"] | (doc["hostname"] | "Apex");

  const Settings &s = settings::get();
  int n = 0;
  for (JsonObjectConst i : arr) {
    String name = i["name"] | "";
    if (!name.length()) continue;
    String type = i["type"] | "";
    float v = parseValue(i["value"]);
    String kind = readings::guessKind(type, name);
    if (kind == "temp" && !isnan(v) && reportsF(i["value"], v, s.apexTempUnit)) v = (v - 32.0f) * 5.0f / 9.0f;
    n++;
    if (publish) readings::update("apex:" + name, "Apex", dev, name, kind, readings::unitFor(kind), v);
  }
  if (count) *count = n;
  return "";
}

static void pollJob(String host, String user, String pass) {
  int n = 0;
  String err = poll(host, user, pass, true, &n);
  bool good = !err.length();
  Serial.printf("[apex] poll %s: %s (8-bit free %u, largest %u)\n", host.c_str(),
                good ? (String(n) + " inputs").c_str() : err.c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (good) {
    inputs = n;
    lastOk = millis();
    String st = "ok · " + String(n) + " inputs";
    if (readings::dropped()) st += " · some not kept (panel full)";
    state.set(st);
  } else {
    state.set(err);
  }
  okNow = good;
}

void loop() {
  const Settings &s = settings::get();
  if (!s.apexOn || s.mode != "direct") {
    okNow = false;
    if (shown != (int8_t)s.apexOn) {
      shown = (int8_t)s.apexOn;
      state.set(s.apexOn ? "on (used in direct mode)" : "off");
    }
    lastPoll = 0;
    force = true;
    return;
  }
  shown = -1;
  uint32_t every = (uint32_t)s.apexPoll_s * 1000UL;
  if (!force && millis() - lastPoll < every && lastPoll) return;
  if (worker::pending("apex")) return;
  force = false;
  lastPoll = millis();
  String host = s.apexHost, user = s.apexUser, pass = s.apexPass;
  worker::post("apex", [host, user, pass] { pollJob(host, user, pass); });
}

void pollNow() { force = true; }

String test(const String &host, const String &user, const String &pass) {
  int n = 0;
  String err = poll(host, user, pass, false, &n);
  if (err.length()) return err;
  return "ok · " + String(n) + " inputs found";
}

String status() {
  if (okNow && lastOk) return state.get() + " · " + String((uint32_t)max<int32_t>(0, (int32_t)(millis() - lastOk)) / 1000) + " s ago";
  return state.get();
}
bool ok() { return okNow; }
uint32_t lastOkMs() { return lastOk; }

}  // namespace apex
