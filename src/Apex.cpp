#include "Apex.h"
#include "Log.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <functional>
#include <memory>
#include <vector>

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
  uint32_t atS;  // when mDNS answered (epoch s): asked again after an hour
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
  uint32_t nowS = (uint32_t)time(nullptr);
  bool old = nowS > 1700000000 && (known.atS > nowS || nowS - known.atS > 3600);
  if (known.magic != kKnownMagic || strcmp(known.name, name.c_str()) || !known.ip || old) {
    IPAddress ip = MDNS.queryHost(name.substring(0, name.length() - 6), 3000);
    if ((uint32_t)ip == 0) {
      plog.printf("[apex] mDNS: no answer for %s\n", name.c_str());
      return url;  // let the request fail with its own error
    }
    plog.printf("[apex] %s is %s\n", name.c_str(), ip.toString().c_str());
    known.magic = kKnownMagic;
    strlcpy(known.name, name.c_str(), sizeof known.name);
    known.ip = (uint32_t)ip;
    known.atS = nowS;
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
    // Not the Apex answering (or nobody): ask mDNS again next time; the
    // address may have moved to another device.
    known.ip = 0;
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

// ── History from the Apex's own logs ──────────────────────────────────────
// On battery the panel only looks every 15 min (or once a day), so its own
// sparklines were coarse and its Outlets timeline missed changes. The Apex
// logs inputs (~every 10 min) and output changes itself: read those every
// 30 min and use them instead. Both replies are big (a day of 20 inputs is
// ~150 kB), so they're read one record at a time from the stream.
RTC_DATA_ATTR static uint32_t logsAtS;  // epoch s of the last read (kept through sleep)
constexpr uint32_t kLogsEveryS = 30 * 60;
constexpr int kSlots = panel::kSparkLen;  // 48 x 30 min = 24 h
constexpr uint32_t kSlotS = 1800;

static String logUrl(const String &hostIn, const char *what) {
  String host = hostIn;
  host.trim();
  if (!host.startsWith("http")) host = "http://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);
  host = resolveLocal(host);
  // sdate (YYMMDD) is the start; days=2 covers yesterday and today.
  time_t y = time(nullptr) - 86400;
  struct tm tm;
  localtime_r(&y, &tm);
  char q[64];
  snprintf(q, sizeof q, "/rest/%s?days=2&sdate=%02d%02d%02d&_=%lu", what, tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday,
           (unsigned long)millis());
  return host + q;
}

// Calls fn(record) for each object in the reply's "record" array. Returns
// records read, or -1 with err set.
static int eachRecord(const String &url, const String &user, const String &pass, JsonDocument &filter,
                      std::function<void(JsonObjectConst)> fn, String &err) {
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding: the stream is the body
  http.setTimeout(15000);
  http.setConnectTimeout(5000);
  if (!http.begin(url)) {
    err = "bad address";
    return -1;
  }
  http.setAuthorization(user.c_str(), pass.c_str());
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != 200) {
    http.end();
    err = "HTTP " + String(code);
    known.ip = 0;
    return -1;
  }
  Stream &st = http.getStream();
  st.setTimeout(8000);
  if (!st.find("\"record\"") || !st.find('[')) {
    http.end();
    return 0;  // no records (logging off, or nothing yet)
  }
  int n = 0;
  JsonDocument doc;
  uint32_t t0 = millis();
  bool done = false;
  while (millis() - t0 < 40000) {
    int c = -1;
    uint32_t w = millis();
    while ((c = st.peek()) == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t' || (c < 0 && millis() - w < 8000)) {
      if (c < 0) delay(5);
      else st.read();
    }
    if (c == ']') {
      done = true;  // the whole log arrived
      break;
    }
    if (c != '{') {
      err = c < 0 ? "reply stopped" : "unexpected reply";
      break;
    }
    DeserializationError e = deserializeJson(doc, st, DeserializationOption::Filter(filter));
    if (e) {
      err = String("bad record: ") + e.c_str();
      break;
    }
    fn(doc.as<JsonObjectConst>());
    n++;
  }
  if (!done && !err.length()) err = "timed out";
  http.end();
  return n;
}

static void readLogs(const String &host, const String &user, const String &pass) {
  uint32_t nowS = (uint32_t)time(nullptr);
  if (nowS < 1700000000) return;
  // Copies: a settings save on the loop may replace these while this runs.
  String tempUnit = settings::get().apexTempUnit;
  std::vector<String> picked;
  for (auto &o : settings::get().outlets)
    if (o.key.startsWith("apex:")) picked.push_back(o.key);
  uint32_t t0 = millis();

  // Inputs: per-reading sums for each 30-min slot of the last 24 h.
  struct Acc {
    char key[48];
    char kind[8];
    float sum[kSlots];
    uint16_t n[kSlots];
  };
  int nk = readings::count(), napex = 0;
  for (int i = 0; i < nk; i++) {
    readings::Reading r;
    if (readings::at(i, r) && !strncmp(r.key, "apex:", 5)) napex++;
  }
  if (!napex) return;
  std::unique_ptr<Acc[]> acc(new (std::nothrow) Acc[napex]);
  if (!acc) {
    plog.println("[apex] input log: no memory");
    return;
  }
  int na = 0;
  for (int i = 0; i < nk && na < napex; i++) {
    readings::Reading r;
    if (!readings::at(i, r) || strncmp(r.key, "apex:", 5)) continue;
    Acc &a = acc[na++];
    memset(&a, 0, sizeof a);
    strlcpy(a.key, r.key, sizeof a.key);
    strlcpy(a.kind, r.kind, sizeof a.kind);
  }
  JsonDocument f;
  f["date"] = true;
  f["data"][0]["name"] = true;
  f["data"][0]["value"] = true;
  String err;
  uint32_t newest = 0;
  int outside = 0;
  int recs = eachRecord(logUrl(host, "ilog"), user, pass, f, [&](JsonObjectConst rec) {
    uint32_t t = rules::apexDate(rec["date"].is<double>() ? rec["date"].as<double>() : 0, rec["date"] | "");
    if (t > newest) newest = t;
    int k = rules::slotFor(t, nowS, kSlots, kSlotS);
    if (k < 0) {
      outside++;
      return;
    }
    for (JsonObjectConst d : rec["data"].as<JsonArrayConst>()) {
      const char *name = d["name"] | "";
      char key[48];
      snprintf(key, sizeof key, "apex:%s", name);
      for (int i = 0; i < na; i++) {
        if (strcmp(acc[i].key, key)) continue;
        float v = parseValue(d["value"]);
        if (isnan(v)) break;
        if (!strcmp(acc[i].kind, "temp") && reportsF(d["value"], v, tempUnit)) v = (v - 32.0f) * 5.0f / 9.0f;
        acc[i].sum[k] += v;
        acc[i].n[k]++;
        break;
      }
    }
  }, err);
  int filled = 0;
  float pts[kSlots];
  // Only a whole, current log replaces the trend: a cut-off read lacks the
  // newest records, and carrying the last value forward would draw them flat.
  bool usable = !err.length() && newest && nowS - newest < 45 * 60;
  for (int i = 0; usable && i < na; i++) {
    int np = rules::slotPoints(acc[i].sum, acc[i].n, kSlots, pts);
    if (np >= 2 && readings::setHistory(acc[i].key, pts, np, 0)) filled++;
  }
  acc.reset();
  plog.printf("[apex] input log: %d records, %d readings given 24 h history%s%s (newest %ld s ago, %d outside the day)\n",
                recs, filled, err.length() ? ", " : "", err.c_str(), newest ? (long)nowS - (long)newest : -1L, outside);

  // Outputs: on/off changes for the picked Apex outputs.
  struct Ev {
    char key[44];
    uint8_t n;
    uint32_t at[46];
    uint8_t lvl[46];
  };
  std::vector<Ev> evs;
  for (auto &k : picked) {
    Ev e;
    memset(&e, 0, sizeof e);
    strlcpy(e.key, k.c_str(), sizeof e.key);
    evs.push_back(e);
  }
  if (!evs.empty()) {
    JsonDocument g;
    g["date"] = true;
    g["name"] = true;
    g["value"] = true;
    err = "";
    int changes = 0;
    int orecs = eachRecord(logUrl(host, "olog"), user, pass, g, [&](JsonObjectConst rec) {
      uint32_t t = rules::apexDate(rec["date"].is<double>() ? rec["date"].as<double>() : 0, rec["date"] | "");
      if (!t || t > nowS + 300 || nowS - t > 26UL * 3600UL) return;
      char key[48];
      snprintf(key, sizeof key, "apex:%s", (const char *)(rec["name"] | ""));
      String v = rec["value"].is<const char *>() ? String(rec["value"].as<const char *>()) : String(rec["value"] | 0);
      for (auto &e : evs) {
        if (strcmp(e.key, key)) continue;
        if (e.n == 46) {  // keep the newest
          memmove(e.at, e.at + 1, sizeof e.at[0] * 45);
          memmove(e.lvl, e.lvl + 1, 45);
          e.n = 45;
        }
        e.at[e.n] = t;
        e.lvl[e.n++] = rules::apexOn(v.c_str()) ? 100 : 0;
        changes++;
        break;
      }
    }, err);
    int lanes = 0;
    for (auto &e : evs)
      if (!err.length() && e.n && outlets::setHistory(e.key, e.at, e.lvl, e.n)) lanes++;
    plog.printf("[apex] output log: %d records, %d changes for %d picked output(s)%s%s\n", orecs, changes, lanes,
                  err.length() ? ", " : "", err.c_str());
  }
  logsAtS = usable ? nowS : nowS - kLogsEveryS + 300;  // a failed read tries again in 5 min
  plog.printf("[apex] logs read in %lu ms\n", (unsigned long)(millis() - t0));
}

static void pollJob(String host, String user, String pass) {
  int n = 0;
  String err = poll(host, user, pass, true, &n);
  bool good = !err.length();
  plog.printf("[apex] poll %s: %s (8-bit free %u, largest %u)\n", host.c_str(),
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
  uint32_t nowS = (uint32_t)time(nullptr);
  // Its own job, so a Hydros renewal or an update check queued meanwhile
  // isn't held up behind it.
  if (good && nowS > 1700000000 && (!logsAtS || nowS - logsAtS >= kLogsEveryS || logsAtS > nowS))
    worker::post("apex-logs", [host, user, pass] { readLogs(host, user, pass); });
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
