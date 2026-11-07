#include "Apex.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "Net.h"
#include "Outlets.h"
#include "Rules.h"
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
static volatile uint8_t fails = 0;  // polls failed in a row: retry sooner than poll_s

// The Apex's address from mDNS, kept through battery sleeps so a wake doesn't
// spend a query on it. Cleared when a request to it fails (it may have moved).
struct Resolved {
  uint32_t magic;
  char name[40];
  uint32_t ip;
};
RTC_DATA_ATTR static Resolved known;
constexpr uint32_t kKnownMagic = 0xA9E70001;

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

// "http://apex.local[:port]" -> "http://10.0.0.227[:port]". The ESP32's DNS
// lookup doesn't do mDNS, so .local names fail with "connection refused"
// (2026-11-06, the Apex's default name). Asks mDNS instead; keeps the last
// answer for 10 min. Anything else is returned unchanged.
static String resolveLocal(const String &url) {
  int a = url.indexOf("://") + 3;
  int b = a;
  while (b < (int)url.length() && url[b] != ':' && url[b] != '/') b++;
  String name = url.substring(a, b);
  if (!name.endsWith(".local")) return url;
  if (known.magic != kKnownMagic || strcmp(known.name, name.c_str()) || !known.ip) {
    IPAddress ip = MDNS.queryHost(name.substring(0, name.length() - 6), 3000);
    if ((uint32_t)ip == 0) {
      Serial.printf("[apex] mDNS: no answer for %s\n", name.c_str());
      return url;  // let the request fail with its own error
    }
    Serial.printf("[apex] %s is %s\n", name.c_str(), ip.toString().c_str());
    known.magic = kKnownMagic;
    strlcpy(known.name, name.c_str(), sizeof known.name);
    known.ip = (uint32_t)ip;
  }
  return url.substring(0, a) + IPAddress(known.ip).toString() + url.substring(b);
}

// Returns "" on success, else an error. publish = write to readings.
static String poll(const String &hostIn, const String &user, const String &pass, bool publish, int *count) {
  if (!WiFi.isConnected()) return "no Wi-Fi";
  String host = hostIn;
  host.trim();
  if (!host.length()) return "no Apex address set";
  if (!host.startsWith("http")) host = "http://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);
  host = resolveLocal(host);

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
    if (code < 0) known.ip = 0;  // ask mDNS again next time
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
  JsonObject out = filter["outputs"][0].to<JsonObject>();
  out["name"] = true;
  out["type"] = true;
  out["status"] = true;
  filter["istat"]["outputs"][0] = out;
  filter["status"]["outputs"][0] = out;
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

  // Outputs: status[0] is ON / OFF (forced by hand), AON / AOF (auto), or a
  // profile name state like TBL for variable outputs.
  JsonArrayConst outs = doc["outputs"].as<JsonArrayConst>();
  if (outs.isNull()) outs = doc["istat"]["outputs"].as<JsonArrayConst>();
  if (outs.isNull()) outs = doc["status"]["outputs"].as<JsonArrayConst>();
  if (publish && !outs.isNull()) {
    for (JsonObjectConst o : outs) {
      String name = o["name"] | "";
      if (!name.length()) continue;
      String type = o["type"] | "";
      String st = o["status"][0] | "";
      bool on, manual, hidden;
      rules::apexOutput(st.c_str(), type.c_str(), on, manual, hidden);
      outlets::update(("apex:" + name).c_str(), "Apex", type.c_str(), on ? 100 : 0, manual, hidden);
    }
  }
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
  fails = good ? 0 : (fails < 10 ? fails + 1 : fails);
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
  // Not before the panel has an address: a poll then fails with "no Wi-Fi" and
  // the next one waited the whole poll_s (Apex took a minute to show after a
  // wake, 2026-11-07).
  if (!net::online()) return;
  uint32_t every = (uint32_t)s.apexPoll_s * 1000UL;
  // After a failure try again soon: 5, 10, 20, 40 s ... up to poll_s.
  if (fails) every = min<uint32_t>(every, 5000UL << min<int>(fails - 1, 4));
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
