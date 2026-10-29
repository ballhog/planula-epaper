#include "Readings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace readings {

static std::vector<Reading> items;
static SemaphoreHandle_t mu = nullptr;
static constexpr uint32_t kSlotMs = 30UL * 60UL * 1000UL;

struct Lock {
  Lock() {
    if (!mu) mu = xSemaphoreCreateMutex();
    xSemaphoreTake(mu, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(mu); }
};

static void pushSpark(Reading &r, float v) {
  if (r.nspark < panel::kSparkLen) r.spark[r.nspark++] = v;
  else {
    memmove(r.spark, r.spark + 1, sizeof(float) * (panel::kSparkLen - 1));
    r.spark[panel::kSparkLen - 1] = v;
  }
}

void update(const String &key, const String &source, const String &device, const String &name, const String &kind,
            const String &unit, float value) {
  Lock l;
  Reading *r = nullptr;
  for (auto &it : items)
    if (it.key == key) { r = &it; break; }
  if (!r) {
    if (items.size() >= 80) return;
    items.emplace_back();
    r = &items.back();
    r->key = key;
    r->slotStartMs = millis();
  }
  r->source = source;
  r->device = device;
  r->name = name;
  r->kind = kind;
  r->unit = unit;
  r->value = value;
  r->atMs = millis();
  if (isnan(value)) return;
  // The trend's newest point follows the live value; a new slot starts every 30 min.
  if (r->nspark == 0) {
    pushSpark(*r, value);
    r->slotStartMs = millis();
    r->slotSum = value;
    r->slotN = 1;
    return;
  }
  if (millis() - r->slotStartMs >= kSlotMs) {
    r->slotStartMs = millis();
    r->slotSum = 0;
    r->slotN = 0;
    pushSpark(*r, value);
  }
  r->slotSum += value;
  r->slotN++;
  r->spark[r->nspark - 1] = r->slotSum / r->slotN;
}

bool get(const String &key, Reading &out) {
  Lock l;
  for (auto &it : items)
    if (it.key == key) { out = it; return true; }
  return false;
}

std::vector<Reading> all() {
  Lock l;
  return items;
}

void clearSource(const String &source) {
  Lock l;
  for (size_t i = 0; i < items.size();)
    if (items[i].source == source) items.erase(items.begin() + i);
    else i++;
}

String guessKind(const String &type, const String &name) {
  String t = type, n = name;
  t.toLowerCase();
  n.toLowerCase();
  if (t == "temp" || n.indexOf("temp") >= 0 || n.indexOf("tmp") >= 0) return "temp";
  if (t == "ph" || n == "ph" || n.startsWith("ph ") || n.endsWith(" ph") || n.indexOf("ph") == 0) return "ph";
  if (t == "orp" || n.indexOf("orp") >= 0 || n.indexOf("redox") >= 0) return "orp";
  if (t == "cond" || t == "salinity" || n.indexOf("cond") >= 0 || n.indexOf("salin") >= 0 || n.indexOf("salt") >= 0)
    return "cond";
  if (t == "alk" || n.indexOf("alk") >= 0) return "alk";
  if (t == "ca" || t == "calc" || n.startsWith("ca") || n.indexOf("calc") >= 0) return "calc";
  if (t == "mg" || t == "mag" || n.startsWith("mg") || n.indexOf("mag") >= 0) return "mag";
  if (n.indexOf("level") >= 0 || n.indexOf("lls") >= 0) return "level";
  if (t == "amps" || n.indexOf("amp") >= 0) return "amps";
  if (t == "pwr" || n.indexOf("watt") >= 0 || n.endsWith("w")) return "watts";
  return "other";
}

String unitFor(const String &kind) {
  if (kind == "temp") return "°C";
  if (kind == "orp") return "mV";
  if (kind == "cond") return "ppt";
  if (kind == "alk") return "dKH";
  if (kind == "calc" || kind == "mag") return "ppm";
  if (kind == "amps") return "A";
  if (kind == "watts") return "W";
  return "";
}

}  // namespace readings
