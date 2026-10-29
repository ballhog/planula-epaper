#include "Readings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace readings {

static Reading items[kMax];
static int n = 0;
static SemaphoreHandle_t mu = nullptr;
static constexpr uint32_t kSlotMs = 30UL * 60UL * 1000UL;

struct Lock {
  Lock() {
    if (!mu) mu = xSemaphoreCreateMutex();
    xSemaphoreTake(mu, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(mu); }
};

static uint32_t nDropped = 0;
static volatile uint32_t nNew = 0;

// float <-> IEEE 754 half (round to nearest, no NaN/denormal subtleties needed)
static uint16_t toHalf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  uint16_t sign = (x >> 16) & 0x8000;
  int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
  uint32_t m = x & 0x7FFFFF;
  if (e <= 0) return sign;                // too small: 0
  if (e >= 31) return sign | 0x7BFF;      // too big: clamp to 65504
  uint16_t h = sign | (uint16_t)(e << 10) | (uint16_t)(m >> 13);
  if (m & 0x1000) h++;                     // round
  return h;
}

static float fromHalf(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (!e) return sign ? -0.0f : 0.0f;
  uint32_t x = sign | ((e - 15 + 127) << 23) | (m << 13);
  float f;
  memcpy(&f, &x, 4);
  return f;
}

float sparkAt(const Reading &r, int i) { return fromHalf(r.spark[i]); }
uint32_t dropped() { return nDropped; }
uint32_t newKeys() { return nNew; }

static void pushSpark(Reading &r, float v) {
  if (r.nspark < panel::kSparkLen) r.spark[r.nspark++] = toHalf(v);
  else {
    memmove(r.spark, r.spark + 1, sizeof(r.spark[0]) * (panel::kSparkLen - 1));
    r.spark[panel::kSparkLen - 1] = toHalf(v);
  }
}

void update(const String &key, const char *source, const String &device, const String &name, const String &kind,
            const String &unit, float value) {
  Lock l;
  Reading *r = nullptr;
  for (int i = 0; i < n; i++)
    if (!strcmp(items[i].key, key.c_str())) { r = &items[i]; break; }
  if (!r) {
    if (n >= kMax) {
      if (nDropped++ % 50 == 0) Serial.printf("[readings] table full (%d), dropped %s\n", kMax, key.c_str());
      return;
    }
    r = &items[n++];
    memset(r, 0, sizeof *r);
    nNew++;
    PANEL_COPY(r->key, key.c_str());
    r->slotStartMs = millis();
  }
  PANEL_COPY(r->source, source);
  PANEL_COPY(r->device, device.c_str());
  PANEL_COPY(r->name, name.c_str());
  PANEL_COPY(r->kind, kind.c_str());
  PANEL_COPY(r->unit, unit.c_str());
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
  r->spark[r->nspark - 1] = toHalf(r->slotSum / r->slotN);
}

bool get(const String &key, Reading &out) {
  Lock l;
  for (int i = 0; i < n; i++)
    if (!strcmp(items[i].key, key.c_str())) {
      out = items[i];
      return true;
    }
  return false;
}

int count() {
  Lock l;
  return n;
}

bool at(int i, Reading &out) {
  Lock l;
  if (i < 0 || i >= n) return false;
  out = items[i];
  return true;
}

void clearSource(const char *source) {
  Lock l;
  int w = 0;
  for (int i = 0; i < n; i++)
    if (strcmp(items[i].source, source)) items[w++] = items[i];
  n = w;
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
