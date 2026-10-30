#include "Settings.h"

#include <Preferences.h>
#include <esp_random.h>

#include <memory>

namespace settings {

static Settings s;
static const char *NS = "planula";

Settings &get() { return s; }

static void putF(JsonObject o, const char *k, float v) {
  if (isnan(v)) o[k] = nullptr;
  else o[k] = v;
}
static float getF(JsonVariantConst v, float def) {
  if (v.isNull()) return def;
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  if (v.is<const char *>()) {
    const char *t = v.as<const char *>();
    if (!t[0]) return NAN;
    return atof(t);
  }
  return def;
}

static void secret(JsonObject o, const char *k, const String &v, bool with) {
  if (with) o[k] = v;
  else o[k]["set"] = v.length() > 0;
}
static void applySecret(JsonVariantConst v, String &dst) {
  if (v.is<const char *>()) {
    const char *t = v.as<const char *>();
    if (t[0]) dst = t;  // "" keeps the old one
  } else if (v["clear"] | false) {
    dst = "";
  }
}

void toJson(JsonObject o, bool with) {
  o["name"] = s.name;
  o["mode"] = s.mode;
  JsonArray w = o["wifi"].to<JsonArray>();
  for (auto &n : s.wifi) {
    JsonObject j = w.add<JsonObject>();
    j["ssid"] = n.ssid;
    secret(j, "pass", n.pass, with);
  }
  JsonObject c = o["coeno"].to<JsonObject>();
  c["host"] = s.coenoHost;
  c["port"] = s.coenoPort;
  c["user"] = s.coenoUser;
  secret(c, "pass", s.coenoPass, with);
  c["updates"] = s.coenoUpdates;
  JsonObject u = o["updates"].to<JsonObject>();
  u["github"] = s.githubUpdates;
  u["auto"] = s.autoUpdate;
  JsonObject a = o["apex"].to<JsonObject>();
  a["on"] = s.apexOn;
  a["host"] = s.apexHost;
  a["user"] = s.apexUser;
  secret(a, "pass", s.apexPass, with);
  a["poll_s"] = s.apexPoll_s;
  a["temp_unit"] = s.apexTempUnit;
  JsonObject h = o["hydros"].to<JsonObject>();
  h["on"] = s.hydrosOn;
  h["user"] = s.hydrosUser;
  secret(h, "pass", s.hydrosPass, with);
  JsonObject d = o["display"].to<JsonObject>();
  d["tz"] = s.tz;
  d["clock24"] = s.clock24;
  d["temp_f"] = s.tempF;
  d["rotate_s"] = s.rotate_s;
  d["stale_s"] = s.stale_s;
  d["full_every"] = s.fullEvery;
  d["driver"] = s.panelDriver;
  JsonArray p = o["pages"].to<JsonArray>();
  for (auto &pg : s.pages) {
    JsonObject j = p.add<JsonObject>();
    j["title"] = pg.title;
    j["layout"] = pg.layout;
  }
  JsonArray g = o["gauges"].to<JsonArray>();
  for (auto &gc : s.gauges) {
    JsonObject j = g.add<JsonObject>();
    j["key"] = gc.key;
    j["label"] = gc.label;
    j["unit"] = gc.unit;
    j["dec"] = gc.dec;
    j["page"] = gc.page;
    putF(j, "warn_lo", gc.warnLo);
    putF(j, "warn_hi", gc.warnHi);
    putF(j, "alarm_lo", gc.alarmLo);
    putF(j, "alarm_hi", gc.alarmHi);
  }
  secret(o, "admin_pass", s.adminPass, with);
  if (with) o["ap_pass"] = s.apPass;
}

static bool validName(const String &n) {
  if (n.length() < 1 || n.length() > 31) return false;
  for (char ch : n)
    if (!(isalnum((unsigned char)ch) || ch == '-')) return false;
  return true;
}

String applyJson(JsonObjectConst o) {
  if (o["name"].is<const char *>()) {
    String n = o["name"].as<const char *>();
    n.toLowerCase();
    if (!validName(n)) return "name: letters, digits and '-' only";
    s.name = n;
  }
  if (o["mode"].is<const char *>()) {
    String m = o["mode"].as<const char *>();
    if (m != "coeno" && m != "direct") return "mode must be coeno or direct";
    s.mode = m;
  }
  if (o["wifi"].is<JsonArrayConst>()) {
    std::vector<WifiNet> nw;
    for (JsonObjectConst j : o["wifi"].as<JsonArrayConst>()) {
      WifiNet n;
      n.ssid = j["ssid"] | "";
      if (!n.ssid.length()) continue;
      // Keep the saved password when the page sends the network back without one.
      for (auto &old : s.wifi)
        if (old.ssid == n.ssid) n.pass = old.pass;
      applySecret(j["pass"], n.pass);
      nw.push_back(n);
      if (nw.size() >= 6) break;
    }
    s.wifi = nw;
  }
  if (o["coeno"].is<JsonObjectConst>()) {
    JsonObjectConst c = o["coeno"];
    if (c["host"].is<const char *>()) s.coenoHost = c["host"].as<const char *>();
    if (c["port"].is<int>()) s.coenoPort = c["port"];
    if (c["user"].is<const char *>()) s.coenoUser = c["user"].as<const char *>();
    applySecret(c["pass"], s.coenoPass);
    if (c["updates"].is<bool>()) s.coenoUpdates = c["updates"];
  }
  if (o["updates"].is<JsonObjectConst>()) {
    JsonObjectConst u = o["updates"];
    if (u["github"].is<bool>()) s.githubUpdates = u["github"];
    if (u["auto"].is<bool>()) s.autoUpdate = u["auto"];
  }
  if (o["apex"].is<JsonObjectConst>()) {
    JsonObjectConst a = o["apex"];
    if (a["on"].is<bool>()) s.apexOn = a["on"];
    if (a["host"].is<const char *>()) s.apexHost = a["host"].as<const char *>();
    if (a["user"].is<const char *>()) s.apexUser = a["user"].as<const char *>();
    applySecret(a["pass"], s.apexPass);
    if (a["poll_s"].is<int>()) s.apexPoll_s = constrain((int)a["poll_s"], 10, 3600);
    if (a["temp_unit"].is<const char *>()) s.apexTempUnit = a["temp_unit"].as<const char *>();
  }
  if (o["hydros"].is<JsonObjectConst>()) {
    JsonObjectConst h = o["hydros"];
    if (h["on"].is<bool>()) s.hydrosOn = h["on"];
    if (h["user"].is<const char *>()) s.hydrosUser = h["user"].as<const char *>();
    applySecret(h["pass"], s.hydrosPass);
  }
  if (o["display"].is<JsonObjectConst>()) {
    JsonObjectConst d = o["display"];
    if (d["tz"].is<const char *>()) s.tz = d["tz"].as<const char *>();
    if (d["clock24"].is<bool>()) s.clock24 = d["clock24"];
    if (d["temp_f"].is<bool>()) s.tempF = d["temp_f"];
    if (d["rotate_s"].is<int>()) s.rotate_s = constrain((int)d["rotate_s"], 0, 3600);
    if (d["stale_s"].is<int>()) s.stale_s = constrain((int)d["stale_s"], 60, 86400);
    if (d["full_every"].is<int>()) s.fullEvery = constrain((int)d["full_every"], 1, 200);
    if (d["driver"].is<const char *>()) {
      String v = d["driver"].as<const char *>();
      if (v == "t7" || v == "gdey") s.panelDriver = v;
    }
  }
  if (o["pages"].is<JsonArrayConst>()) {
    std::vector<PageCfg> np;
    for (JsonObjectConst j : o["pages"].as<JsonArrayConst>()) {
      PageCfg p;
      p.title = j["title"] | "Page";
      p.layout = j["layout"] | "auto";
      np.push_back(p);
      if (np.size() >= 6) break;
    }
    s.pages = np;
  }
  if (o["gauges"].is<JsonArrayConst>()) {
    std::vector<GaugeCfg> ng;
    for (JsonObjectConst j : o["gauges"].as<JsonArrayConst>()) {
      GaugeCfg g;
      g.key = j["key"] | "";
      if (!g.key.length()) continue;
      g.label = j["label"] | "";
      g.unit = j["unit"] | "";
      g.dec = constrain((int)(j["dec"] | 1), 0, 4);
      g.page = constrain((int)(j["page"] | 0), 0, 5);
      g.warnLo = getF(j["warn_lo"], NAN);
      g.warnHi = getF(j["warn_hi"], NAN);
      g.alarmLo = getF(j["alarm_lo"], NAN);
      g.alarmHi = getF(j["alarm_hi"], NAN);
      ng.push_back(g);
      if (ng.size() >= 40) break;
    }
    s.gauges = ng;
  }
  if (o["admin_pass"].is<const char *>() || o["admin_pass"].is<JsonObjectConst>()) applySecret(o["admin_pass"], s.adminPass);
  return "";
}

static String makeApPass() {
  // Easy to type off the screen: no 0/O/1/l.
  const char *al = "abcdefghjkmnpqrstuvwxyz23456789";
  String p = "reef-";
  for (int i = 0; i < 6; i++) p += al[esp_random() % strlen(al)];
  return p;
}

void load() {
  Preferences p;
  String js;
  if (p.begin(NS, true)) {
    size_t n = p.getBytesLength("cfg");
    if (n > 0 && n < 16384) {
      std::unique_ptr<char[]> buf(new char[n + 1]);
      p.getBytes("cfg", buf.get(), n);
      buf[n] = 0;
      js = buf.get();
    }
    p.end();
  }
  if (js.length()) {
    JsonDocument doc;
    if (!deserializeJson(doc, js)) {
      applyJson(doc.as<JsonObjectConst>());
      s.apPass = doc["ap_pass"] | "";
    }
  }
  if (s.pages.empty()) s.pages.push_back(PageCfg());
  if (s.apPass.length() < 8) {
    s.apPass = makeApPass();
    save();
  }
}

bool save() {
  JsonDocument doc;
  toJson(doc.to<JsonObject>(), true);
  String out;
  serializeJson(doc, out);
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.putBytes("cfg", out.c_str(), out.length()) == out.length();
  p.end();
  return ok;
}

void factoryReset() {
  Preferences p;
  if (p.begin(NS, false)) {
    p.clear();
    p.end();
  }
  s = Settings();
}

}  // namespace settings
