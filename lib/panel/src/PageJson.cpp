#include "PageJson.h"

#include <ArduinoJson.h>
#include <math.h>

namespace panel {

namespace {

bool isNum(JsonVariant v) { return v.is<float>() || v.is<int>(); }

void addPoint(Gauge &g, long t, float v) {
  if (g.nspark >= kSparkLen) return;
  if (t < -32000) t = -32000;
  if (t > 32000) t = 32000;
  // keep oldest first even if the sender didn't
  int i = g.nspark++;
  while (i > 0 && g.t[i - 1] > t) {
    g.t[i] = g.t[i - 1];
    g.spark[i] = g.spark[i - 1];
    i--;
  }
  g.t[i] = (int16_t)t;
  g.spark[i] = v;
}

void parseLane(JsonObject j, Gauge &g) {
  g.lane = laneFromName(j["kind"] | "level");
  g.nspark = 0;  // "spark" means nothing on a lane
  for (JsonVariant p : j["pts"].as<JsonArray>()) {
    JsonArray a = p.as<JsonArray>();
    if (a.isNull() || !isNum(a[0])) continue;
    addPoint(g, a[0].as<long>(), isNum(a[1]) ? a[1].as<float>() : NAN);
  }
  for (JsonVariant p : j["spans"].as<JsonArray>()) {
    JsonArray a = p.as<JsonArray>();
    if (a.isNull() || !isNum(a[0]) || !isNum(a[1]) || g.nspark + 2 > kSparkLen) continue;
    g.lane = Lane::OnOff;
    addPoint(g, a[0].as<long>(), 1);
    addPoint(g, a[1].as<long>(), 0);
  }
  // A lane is "there" when it has points, even without a current value.
  if (!g.has && g.nspark && g.status == Status::Missing) g.status = Status::Ok;
}

}  // namespace

const char *parsePage(const char *json, size_t len, Page &out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) return "bad json";
  JsonObject o = doc.as<JsonObject>();
  if (o.isNull()) return "not an object";
  out.clear();
  PANEL_COPY(out.title, o["title"] | "");
  out.layout = layoutFromName(o["layout"] | "auto");
  out.from = o["from"] | -720;
  out.to = o["to"] | 720;
  out.clock = o["clock"].is<int>() ? (int16_t)(((o["clock"].as<int>() % 1440) + 1440) % 1440) : -1;
  out.day = !strcmp(o["window"] | "", "day");
  dayWindow(out);
  if (out.to <= out.from) { out.from = -720; out.to = 720; }
  JsonArray gs = o["gauges"].as<JsonArray>();
  for (JsonObject j : gs) {
    if (out.n >= kMaxGauges) break;
    Gauge &g = out.g[out.n++];
    g = Gauge();
    PANEL_COPY(g.id, j["id"] | "");
    PANEL_COPY(g.label, j["label"] | (const char *)g.id);
    PANEL_COPY(g.unit, j["unit"] | "");
    PANEL_COPY(g.src, j["src"] | "");
    PANEL_COPY(g.note, j["note"] | "");
    g.dec = j["dec"] | 1;
    JsonVariant v = j["value"];
    g.has = v.is<float>() || v.is<int>();
    g.value = g.has ? v.as<float>() : NAN;
    JsonVariant lo = j["lo"], hi = j["hi"];
    g.hasRange = (lo.is<float>() || lo.is<int>()) && (hi.is<float>() || hi.is<int>());
    if (g.hasRange) {
      g.lo = lo.as<float>();
      g.hi = hi.as<float>();
    }
    g.age_s = j["age_s"] | 0;
    g.status = j["status"].is<const char *>() ? statusFromName(j["status"]) : (g.has ? Status::Ok : Status::Missing);
    if (!g.has) g.status = Status::Missing;
    for (JsonVariant s : j["spark"].as<JsonArray>()) {
      if (g.nspark >= kSparkLen) break;
      if (s.is<float>() || s.is<int>()) g.spark[g.nspark++] = s.as<float>();
    }
    if (out.layout == Layout::Timeline) parseLane(j, g);
  }
  return "";
}

void evaluate(Gauge &g, const Limits &l, uint32_t staleAfter_s) {
  g.note[0] = 0;
  g.hasRange = !isnan(l.warnLo) && !isnan(l.warnHi);
  if (g.hasRange) {
    g.lo = l.warnLo;
    g.hi = l.warnHi;
  }
  if (!g.has || isnan(g.value)) {
    g.status = Status::Missing;
    return;
  }
  if (staleAfter_s && g.age_s > staleAfter_s) {
    g.status = Status::Stale;
    return;
  }
  float v = g.value;
  if (!isnan(l.alarmLo) && v < l.alarmLo) { g.status = Status::Alarm; PANEL_COPY(g.note, "LOW"); return; }
  if (!isnan(l.alarmHi) && v > l.alarmHi) { g.status = Status::Alarm; PANEL_COPY(g.note, "HIGH"); return; }
  if (!isnan(l.warnLo) && v < l.warnLo) { g.status = Status::Warn; PANEL_COPY(g.note, "LOW"); return; }
  if (!isnan(l.warnHi) && v > l.warnHi) { g.status = Status::Warn; PANEL_COPY(g.note, "HIGH"); return; }
  g.status = Status::Ok;
}

}  // namespace panel
