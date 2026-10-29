#include "PageJson.h"

#include <ArduinoJson.h>
#include <math.h>

namespace panel {

const char *parsePage(const char *json, size_t len, Page &out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) return "bad json";
  JsonObject o = doc.as<JsonObject>();
  if (o.isNull()) return "not an object";
  out.clear();
  PANEL_COPY(out.title, o["title"] | "");
  out.layout = layoutFromName(o["layout"] | "auto");
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
