#include "Rules.h"

#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

namespace rules {

float redrawStep(const panel::Gauge &g) {
  const char *u = g.unit;
  if (!strcmp(u, "°C")) return 0.1f;
  if (!strcmp(u, "°F")) return 0.2f;
  if (!strcmp(u, "ppt") || !strcmp(u, "dKH") || !strcmp(u, "PSU")) return 0.1f;
  if (!strcmp(u, "mV")) return 5.0f;
  if (!strcmp(u, "%")) return 2.0f;
  if (!strcmp(u, "ppm")) return fmaxf(1.0f, fabsf(g.value) * 0.01f);
  // pH gauges usually have no unit: go by the label, any case ("1R pH", "Sump PH").
  char low[sizeof g.label];
  size_t i = 0;
  for (; g.label[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)g.label[i]);
  low[i] = 0;
  if (!strcmp(u, "pH") || strstr(low, "ph")) return 0.02f;
  float d = powf(10.0f, -(float)g.dec);
  return fmaxf(2.0f * d, fabsf(g.value) * 0.01f);
}

void remember(const panel::Page &p, uint8_t page, Drawn &d) {
  d.n = p.n;
  d.page = page;
  for (int i = 0; i < p.n; i++) {
    d.vals[i] = p.g[i].has ? p.g[i].value : NAN;
    d.stat[i] = (uint8_t)p.g[i].status;
  }
}

void compare(const panel::Page &p, uint8_t page, const Drawn &d, bool &moved, bool &statusChanged) {
  moved = statusChanged = false;
  if (d.n != p.n || d.page != page) {
    moved = statusChanged = true;
    return;
  }
  for (int i = 0; i < p.n; i++) {
    const panel::Gauge &g = p.g[i];
    if ((uint8_t)g.status != d.stat[i]) statusChanged = true;
    float was = d.vals[i];
    if (g.has != !isnan(was)) moved = true;
    else if (g.has && fabsf(g.value - was) >= redrawStep(g) - 1e-6f) moved = true;
  }
}

bool shouldRedraw(Wake why, bool moved, bool statusChanged, uint32_t secondsSinceDraw) {
  if (why == Wake::Daily) return true;
  if (why == Wake::Hourly) return statusChanged;
  return moved || statusChanged || secondsSinceDraw >= 3600;
}

time_t nextAt(time_t now, int hh, int mm) {
  struct tm tm;
  localtime_r(&now, &tm);
  tm.tm_hour = hh;
  tm.tm_min = mm;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;  // let mktime work out DST for that day
  time_t t = mktime(&tm);
  if (t <= now + 30) {
    localtime_r(&now, &tm);
    tm.tm_mday += 1;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
  }
  return t;
}

time_t nextWake(time_t now, bool clockValid, char mode, int saverMin, int dailyH, int dailyM, bool hourly, Wake &why) {
  if (!clockValid) {
    why = Wake::Saver;
    return now + 3600;
  }
  if (mode == 's') {
    why = Wake::Saver;
    return now + (time_t)saverMin * 60;
  }
  time_t at = nextAt(now, dailyH, dailyM);
  why = Wake::Daily;
  if (hourly) {
    time_t hour = (now / 3600 + 1) * 3600 + 20;  // just after the next full hour
    if (hour < at - 120) {
      at = hour;
      why = Wake::Hourly;
    }
  }
  return at;
}

void OutletHistory::record(uint32_t ms, uint8_t level) {
  if (len) {
    uint8_t last = v[len - 1];
    bool onOff = (last == 0) != (level == 0);
    if (!onOff && abs((int)level - (int)last) < 5) return;
  }
  if (len >= kHist) {
    memmove(at, at + 1, sizeof(at[0]) * (kHist - 1));
    memmove(v, v + 1, kHist - 1);
    memmove(ml10, ml10 + 1, sizeof(ml10[0]) * (kHist - 1));
    len = kHist - 1;
  }
  at[len] = ms;
  v[len] = level;
  ml10[len] = 0;
  len++;
}

void OutletHistory::recordDose(uint32_t ms, float ml) {
  if (len && v[len - 1]) record(ms - 1000, 0);  // close an open dose first
  record(ms, 100);
  if (len && v[len - 1] == 100 && at[len - 1] == ms && ml > 0)
    ml10[len - 1] = (uint16_t)(ml * 10.0f + 0.5f > 65535 ? 65535 : ml * 10.0f + 0.5f);
  record(ms + 1000, 0);
}

int lanePoints(const OutletHistory &h, uint32_t nowMs, int fromMin, bool doser, bool variable, int16_t *t, float *val,
               int max) {
  auto ago = [&](int k) { return since(nowMs, h.at[k]) / 60000; };
  int n = 0;
  if (doser) {
    for (int k = 0; k < h.len && n < max; k++) {
      if (h.v[k] == 0 || (k > 0 && h.v[k - 1] > 0)) continue;  // dose starts only
      if (ago(k) > -fromMin) continue;
      t[n] = (int16_t)-ago(k);
      val[n++] = h.ml10[k] ? h.ml10[k] / 10.0f : NAN;
    }
    return n;
  }
  // The state at the window start (the last change before it), then each
  // change inside it.
  int first = h.len;
  for (int k = 0; k < h.len; k++)
    if (ago(k) <= -fromMin) {
      first = k;
      break;
    }
  int start = first > 0 ? first - 1 : 0;
  for (int k = start; k < h.len && n < max; k++) {
    t[n] = (int16_t)(ago(k) > -fromMin ? fromMin : -ago(k));
    val[n++] = variable ? h.v[k] : (h.v[k] ? 1.0f : 0.0f);
  }
  return n;
}

uint8_t hydrosLevel(float valueState, float drain, float fill, bool doser) {
  bool wc = !isnan(drain) || !isnan(fill);
  if (wc) return ((!isnan(drain) && drain > 0) || (!isnan(fill) && fill > 0) || (!isnan(valueState) && valueState > 0)) ? 100 : 0;
  if (isnan(valueState) || valueState <= 0) return 0;
  if (doser) return 100;
  int l = (int)lroundf(valueState / 100.0f);
  return (uint8_t)(l < 1 ? 1 : l > 100 ? 100 : l);
}

void apexOutput(const char *st, const char *type, bool &on, bool &manual, bool &hidden) {
  manual = !strcmp(st, "ON") || !strcmp(st, "OFF");
  on = !strcmp(st, "ON") || !strcmp(st, "AON") || !strcmp(st, "TBL");
  hidden = !strcmp(type, "virtual") || !strcmp(type, "alert") || !strcmp(type, "serial");
}

uint32_t apexDate(double num, const char *str) {
  if (num > 1000000000.0) return (uint32_t)num;
  if (!str) return 0;
  while (*str == ' ') str++;
  // All digits: epoch as a string, or YYYYMMDDHHMM.
  size_t len = 0;
  while (isdigit((unsigned char)str[len])) len++;
  if (len >= 12) {
    struct tm tm = {};
    char b[5] = {0};
    memcpy(b, str, 4);
    tm.tm_year = atoi(b) - 1900;
    char d2[3] = {0};
    memcpy(d2, str + 4, 2); tm.tm_mon = atoi(d2) - 1;
    memcpy(d2, str + 6, 2); tm.tm_mday = atoi(d2);
    memcpy(d2, str + 8, 2); tm.tm_hour = atoi(d2);
    memcpy(d2, str + 10, 2); tm.tm_min = atoi(d2);
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t > 0 ? (uint32_t)t : 0;
  }
  if (len >= 9) return (uint32_t)strtoul(str, nullptr, 10);
  return 0;
}

int slotFor(uint32_t t, uint32_t nowS, int slots, uint32_t slotS) {
  if (t > nowS) return t - nowS < 300 ? slots - 1 : -1;  // a clock a little ahead
  uint32_t ago = nowS - t;
  uint32_t k = ago / slotS;
  if (k >= (uint32_t)slots) return -1;
  return slots - 1 - (int)k;
}

int slotPoints(const float *sum, const uint16_t *n, int slots, float *out) {
  int m = 0;
  float last = NAN;
  for (int i = 0; i < slots; i++) {
    if (n[i]) last = sum[i] / n[i];
    else if (isnan(last)) continue;
    out[m++] = last;
  }
  return m;
}

bool apexOn(const char *st) {
  if (!st) return false;
  if (!strcmp(st, "ON") || !strcmp(st, "AON") || !strcmp(st, "TBL")) return true;
  if (isdigit((unsigned char)st[0])) return atoi(st) > 0;  // some firmware logs 0 / 1
  return false;
}

}  // namespace rules
