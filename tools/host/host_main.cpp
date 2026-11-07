// Host build of lib/panel: renders sample screens to PBM (render.sh turns them
// into PNG) and runs the pure-logic tests. Same drawing code as the panel.
//
//   tools/host/render.sh          → tools/host/out/*.png, then runs the tests
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_GFX.h>

#include <functional>
#include <vector>

#include "BandCanvas.h"
#include "Model.h"
#include "Rules.h"
#include "PageJson.h"
#include "Render.h"
#include "SigV4.h"
#include "Text.h"

using namespace panel;

static int failures = 0;
static void testRules();
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void savePbm(GFXcanvas1 &c, const char *name) {
  char path[256];
  snprintf(path, sizeof path, "out/%s.pbm", name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P4\n%d %d\n", kW, kH);
  fwrite(c.getBuffer(), 1, kW / 8 * kH, f);  // GFXcanvas1: 1 = set = black, same as PBM
  fclose(f);
  printf("wrote %s\n", path);
}

// Draw a screen whole (as before) and in 120-row bands (as the panel does);
// the two must agree bit for bit.
static void shot(GFXcanvas1 &cv, const char *name, const std::function<void(Adafruit_GFX &)> &draw) {
  cv.fillScreen(0);
  draw(cv);
  savePbm(cv, name);
  BandCanvas band(kW, kH, 120);
  int bad = 0;
  for (int16_t y0 = 0; y0 < kH; y0 += band.bandRows()) {
    band.setBand(y0);
    draw(band);
    if (memcmp(band.buffer(), cv.getBuffer() + (size_t)y0 * (kW / 8), band.bandBytes()) != 0) bad++;
  }
  if (bad) printf("FAIL banded drawing differs for %s (%d bands)\n", name, bad);
  failures += bad;
}

static void spark(Gauge &g, float base, float amp, float drift, int n = 48) {
  g.nspark = n;
  for (int i = 0; i < n; i++) g.spark[i] = base + amp * sinf(i / 7.6f * 3.14159f) + drift * i / n;
}

static Gauge mk(const char *id, const char *label, float v, int dec, const char *unit, const char *src, float lo, float hi,
                uint32_t age) {
  Gauge g;
  PANEL_COPY(g.id, id);
  PANEL_COPY(g.label, label);
  PANEL_COPY(g.unit, unit);
  PANEL_COPY(g.src, src);
  g.has = true;
  g.value = v;
  g.dec = dec;
  g.hasRange = true;
  g.lo = lo;
  g.hi = hi;
  g.age_s = age;
  g.status = Status::Ok;
  return g;
}

static void chromeFor(Chrome &c, int page, int pages) {
  PANEL_COPY(c.clock, "8:41");
  c.page = page;
  c.pages = pages;
  PANEL_COPY(c.footerLeft, "Updated 8:33 · next 8:48 · Apex ok · Hydros ok");
  c.wifi = 3;
  c.battery = 96;
  PANEL_COPY(c.room, "72°F  45%");
}

static void samples() {
  GFXcanvas1 cv(kW, kH);
  Chrome c;

  // 1. Tank, 2x2: one warning, one stale.
  Page p;
  PANEL_COPY(p.title, "Tank");
  p.g[0] = mk("tank:ph", "pH", 8.14f, 2, "", "Calyx", 7.9f, 8.4f, 20);
  spark(p.g[0], 8.12f, 0.09f, 0.0f);
  p.g[1] = mk("tank:temp", "Temperature", 25.9f, 1, "°C", "Apex", 25.0f, 26.6f, 40);
  spark(p.g[1], 25.8f, 0.25f, 0.1f);
  p.g[2] = mk("tank:salinity", "Salinity", 35.6f, 1, "ppt", "Calyx", 33.5f, 35.3f, 15);
  spark(p.g[2], 35.0f, 0.15f, 0.6f);
  p.g[2].status = Status::Warn;
  PANEL_COPY(p.g[2].note, "HIGH");
  p.g[3] = mk("tank:orp", "ORP", 312, 0, "mV", "Hydros", 250, 450, 14 * 60);
  spark(p.g[3], 320, 12, -8);
  p.g[3].status = Status::Stale;
  p.n = 4;
  chromeFor(c, 0, 3);
  shot(cv, "1-tank-2x2", [&](Adafruit_GFX &d) { render(d, p, c); });
  {
    Page pg = p;
    pg.view = View::Graphs;
    shot(cv, "15-tank-2x2-graphs", [&](Adafruit_GFX &d) { render(d, pg, c); });
    pg.view = View::Numbers;
    shot(cv, "16-tank-2x2-numbers", [&](Adafruit_GFX &d) { render(d, pg, c); });
  }

  // 2. Six gauges, salinity in alarm: banner + inverted tile.
  Page q;
  PANEL_COPY(q.title, "Chemistry");
  q.g[0] = mk("alk", "Alkalinity", 7.2f, 2, "dKH", "Trident", 7.0f, 8.0f, 3600);
  spark(q.g[0], 7.3f, 0.12f, -0.1f, 24);
  q.g[1] = mk("ca", "Calcium", 452, 0, "ppm", "Trident", 400, 450, 3600);
  spark(q.g[1], 445, 6, 8, 24);
  q.g[1].status = Status::Warn;
  PANEL_COPY(q.g[1].note, "HIGH");
  q.g[2] = mk("mg", "Magnesium", 1372, 0, "ppm", "Trident", 1250, 1400, 3600);
  spark(q.g[2], 1370, 10, 0, 24);
  q.g[3] = mk("sal", "Salinity", 31.2f, 1, "ppt", "Calyx", 33.5f, 35.3f, 12);
  spark(q.g[3], 34.0f, 0.2f, -2.6f);
  q.g[3].status = Status::Alarm;
  PANEL_COPY(q.g[3].note, "LOW");
  q.g[4] = mk("ph", "pH (Hydros)", 8.31f, 2, "", "Hydros", 7.9f, 8.4f, 30);
  spark(q.g[4], 8.2f, 0.1f, 0);
  q.g[5] = mk("co2", "Room CO₂", 1180, 0, "ppm", "Calyx", 400, 1000, 60);
  spark(q.g[5], 900, 120, 200);
  q.g[5].status = Status::Warn;
  PANEL_COPY(q.g[5].note, "HIGH");
  q.n = 6;
  chromeFor(c, 1, 3);
  PANEL_COPY(c.alarm, "SALINITY LOW 31.2 ppt");
  c.alarmCount = 1;
  shot(cv, "2-chem-3x2-alarm", [&](Adafruit_GFX &d) { render(d, q, c); });
  {
    // Direct-mode Outlets page: past only (12 h up to now), "since" times.
    Page o;
    PANEL_COPY(o.title, "Outlets");
    o.layout = Layout::Timeline;
    o.from = -720;
    o.to = 0;
    o.clock = 22 * 60 + 25;
    struct L { const char *name; std::vector<std::pair<int, int>> ev; const char *note; };
    std::vector<L> ls = {
        {"Return", {{-720, 1}, {-200, 0}, {-190, 1}}, ""},
        {"Skimmer", {{-720, 1}, {-200, 0}, {-150, 1}}, ""},
        {"Heater", {{-720, 0}, {-600, 1}, {-560, 0}, {-430, 1}, {-400, 0}, {-250, 1}, {-215, 0}, {-60, 1}}, ""},
        {"Kalk stirrer", {{-720, 0}, {-95, 1}}, "by hand"},
        {"Lights", {{-720, 0}, {-660, 1}, {-120, 0}}, ""},
    };
    // Dimmable output (Hydros valueState) as a level lane, and a dosing head.
    {
      Gauge &g = o.g[o.n++];
      PANEL_COPY(g.label, "1R Blues");
      PANEL_COPY(g.unit, "%");
      g.dec = 0;
      g.lane = Lane::Step;
      g.status = Status::Ok;
      g.has = true;
      int pts[][2] = {{-720, 0}, {-640, 20}, {-620, 45}, {-600, 70}, {-580, 85}, {-180, 60}, {-160, 30}, {-140, 0}};
      for (auto &q : pts) {
        g.t[g.nspark] = q[0];
        g.spark[g.nspark++] = q[1];
      }
      g.value = 0;
    }
    {
      Gauge &g = o.g[o.n++];
      PANEL_COPY(g.label, "1RX10D1-Part C");
      g.lane = Lane::Events;
      g.status = Status::Ok;
      g.has = true;
      for (int t = -690; t < 0; t += 60) {
        g.t[g.nspark] = t;
        g.spark[g.nspark++] = NAN;
      }
    }
    for (auto &l : ls) {
      Gauge &g = o.g[o.n++];
      PANEL_COPY(g.label, l.name);
      PANEL_COPY(g.note, l.note);
      g.lane = Lane::OnOff;
      g.status = Status::Ok;
      g.has = true;
      for (auto &e : l.ev) {
        g.t[g.nspark] = e.first;
        g.spark[g.nspark++] = e.second;
      }
      g.value = g.spark[g.nspark - 1];
    }
    Chrome oc = c;
    oc.page = 2;
    oc.pages = 3;
    shot(cv, "20-outlets", [&](Adafruit_GFX &d) { render(d, o, oc); });
  }
  {
    // Alarm takeover: the chemistry page with its alarm, and with two.
    shot(cv, "18-takeover-one", [&](Adafruit_GFX &d) { render(d, q, c); });
    Page q2 = q;
    for (int k = 0; k < q2.n; k++)
      if (q2.g[k].status != Status::Alarm) {
        q2.g[k].status = Status::Alarm;
        break;
      }
    shot(cv, "19-takeover-two", [&](Adafruit_GFX &d) { render(d, q2, c); });
  }
  {
    // Numbers view on a 3x2 page without alarms (like Joe's Tank page).
    Page qn = q;
    for (int k = 0; k < qn.n; k++)
      if (qn.g[k].status == Status::Alarm) qn.g[k].status = Status::Warn;
    qn.view = View::Numbers;
    shot(cv, "21-chem-3x2-numbers", [&](Adafruit_GFX &d) { render(d, qn, c); });
  }
  {
    Page qg = q;
    qg.view = View::Graphs;
    qg.g[1].nspark = 0;  // one tile with no trend yet
    shot(cv, "17-chem-3x2-graphs", [&](Adafruit_GFX &d) { render(d, qg, c); });
  }
  c.alarmAcked = true;
  shot(cv, "3-chem-3x2-acked", [&](Adafruit_GFX &d) { render(d, q, c); });
  c.alarm[0] = 0;
  c.alarmAcked = false;

  // 4. Single big gauge.
  Page s;
  PANEL_COPY(s.title, "Display tank");
  s.g[0] = mk("tank:ph", "pH", 8.14f, 2, "", "Calyx", 7.9f, 8.4f, 20);
  spark(s.g[0], 8.12f, 0.09f, 0.04f);
  s.n = 1;
  chromeFor(c, 2, 3);
  shot(cv, "4-single", [&](Adafruit_GFX &d) { render(d, s, c); });
  {
    Page sn = s;
    sn.view = View::Numbers;
    shot(cv, "22-single-numbers", [&](Adafruit_GFX &d) { render(d, sn, c); });
    // Two tiles side by side (first two of the tank page), Numbers view.
    Page two = p;
    two.n = 2;
    two.view = View::Numbers;
    shot(cv, "23-two-numbers", [&](Adafruit_GFX &d) { render(d, two, c); });
  }

  // 14. Sparkline y-range from Coeno (spark_lo/spark_hi): the same quiet pH
  // trace drawn without (left, fills the box) and with a 0.4 range (right,
  // draws nearly flat). The right one also has a point outside the range,
  // which widens it rather than being clipped.
  {
    Page sr;
    PANEL_COPY(sr.title, "Spark range");
    sr.layout = Layout::Single;
    sr.g[0] = mk("tank:ph", "pH auto", 8.12f, 2, "", "Calyx", 8.05f, 8.2f, 20);
    spark(sr.g[0], 8.12f, 0.02f, 0.0f);
    sr.g[1] = sr.g[0];
    PANEL_COPY(sr.g[1].label, "pH spark range");
    sr.g[1].hasSparkRange = true;
    sr.g[1].sparkLo = 7.92f;
    sr.g[1].sparkHi = 8.32f;
    sr.g[1].spark[30] = 8.45f;  // outside the range: widens it
    sr.n = 2;
    sr.layout = Layout::Auto;
    chromeFor(c, 0, 1);
    shot(cv, "14-spark-range", [&](Adafruit_GFX &d) { render(d, sr, c); });
  }

  // 5. List of eight.
  Page l;
  PANEL_COPY(l.title, "Everything");
  l.g[0] = p.g[0]; l.g[1] = p.g[1]; l.g[2] = p.g[2]; l.g[3] = p.g[3];
  l.g[4] = q.g[0]; l.g[5] = q.g[1]; l.g[6] = q.g[2]; l.g[7] = q.g[5];
  l.g[8] = mk("flow", "Return flow", 0, 0, "L/h", "Calyx", 4000, 6000, 0);
  l.g[8].has = false;
  l.g[8].status = Status::Missing;
  l.n = 9;
  chromeFor(c, 0, 1);
  shot(cv, "5-list", [&](Adafruit_GFX &d) { render(d, l, c); });

  // 8. Schedule timeline: today, midnight to midnight, "now" 2:20 PM.
  {
    const char *js = R"({"v":1,"title":"Schedule","layout":"timeline","window":"day","clock":860,"gauges":[
      {"id":"apex:Blues","label":"Blues","kind":"level","unit":"%","dec":0,
       "pts":[[-860,0],[-380,0],[-260,85],[310,85],[430,20],[490,0],[580,0]]},
      {"id":"apex:Whites","label":"Whites","kind":"level","unit":"%","dec":0,
       "pts":[[-860,0],[-320,0],[-200,60],[250,60],[370,0],[580,0]]},
      {"id":"apex:Return","label":"Return","kind":"onoff","value":1,
       "spans":[[-860,-375],[-365,580]]},
      {"id":"apex:Skimmer","label":"Skimmer","kind":"onoff","value":1,
       "spans":[[-860,-372],[-357,-130],[-115,175],[190,580]]},
      {"id":"apex:Heater","label":"Heater","kind":"onoff","value":0,
       "spans":[[-800,-760],[-690,-640],[-560,-520],[-430,-405],[150,175],[300,330]]},
      {"id":"e4:alk","label":"Alk doser","kind":"events","unit":"mL","dec":1,
       "pts":[[-800,2.5],[-680,2.5],[-560,2.5],[-440,2.5],[-320,2.5],[-200,2.5],[-80,2.5],[40,2.5],[160,2.5],[280,2.5],[400,2.5],[520,2.5]]},
      {"id":"feed","label":"Feeding","kind":"events","unit":"g","dec":0,
       "pts":[[-372,null],[-130,null],[175,null]]}]})";
    Page tp;
    const char *e = parsePage(js, strlen(js), tp);
    if (e[0]) printf("FAIL timeline sample: %s\n", e), failures++;
    tp.h12 = true;
    chromeFor(c, 0, 4);
    shot(cv, "8-schedule-day", [&](Adafruit_GFX &d) { render(d, tp, c); });
    // Same page, no clock on the page: relative axis, ±12 h.
    const char *js2 = R"({"v":1,"title":"Last & next 6 h","layout":"timeline","from":-360,"to":360,"gauges":[
      {"id":"apex:Blues","label":"Blues","kind":"level","unit":"%","dec":0,"pts":[[-380,0],[-260,85],[310,85],[430,20]]},
      {"id":"apex:ATO","label":"ATO pump","kind":"onoff","value":0,"spans":[[-300,-296],[-190,-186],[-75,-71]]},
      {"id":"e4:alk","label":"Alk doser","kind":"events","dec":1,"pts":[[-320,2.5],[-200,2.5],[-80,2.5],[40,2.5],[160,2.5],[280,2.5]]}]})";
    Page tq;
    e = parsePage(js2, strlen(js2), tq);
    if (e[0]) printf("FAIL timeline sample 2: %s\n", e), failures++;
    shot(cv, "9-schedule-relative", [&](Adafruit_GFX &d) { render(d, tq, c); });
  }

  // 10-13. System screen: update available, installing, up to date, failed.
  {
    SystemInfo si;
    si.build = 12;
    si.version = "v0.4-12-g105a4f8";
    si.availBuild = 15;
    si.availFrom = "Coeno";
    si.detail = "build 15 ready on Coeno";
    si.checked = "checked 2:10 PM";
    const char *k[] = {"ADDRESS", "", "MODE", "WI-FI", "COENO", "UPTIME"};
    const char *v[] = {"planula-epaper-1.local", "10.0.0.87", "Coeno", "HomeNet  ·  -58 dBm",
                       "connected · last page 12 s ago", "3 d 4 h"};
    for (int i = 0; i < 6; i++) si.keys[i] = k[i], si.vals[i] = v[i];
    si.nrows = 6;
    Chrome sc;
    PANEL_COPY(sc.clock, "2:20");
    PANEL_COPY(sc.footerLeft, "Coeno · 10.0.0.228");
    PANEL_COPY(sc.footerRight, "update ready");
    si.state = SystemInfo::Available;
    si.hint = "Press the button to update";
    shot(cv, "10-system-update-available", [&](Adafruit_GFX &d) { renderSystem(d, si, sc); });
    si.state = SystemInfo::Installing;
    si.pct = 40;
    shot(cv, "11-system-updating", [&](Adafruit_GFX &d) { renderSystem(d, si, sc); });
    si.state = SystemInfo::UpToDate;
    si.availBuild = 0;
    si.detail = "up to date (build 12, Coeno)";
    si.hint = "Press: close  ·  hold 3 s: check now";
    sc.footerRight[0] = 0;
    shot(cv, "12-system-up-to-date", [&](Adafruit_GFX &d) { renderSystem(d, si, sc); });
    si.state = SystemInfo::Failed;
    si.availBuild = 15;
    si.detail = "update failed: SHA-256 mismatch";
    si.checked = "still on build 12 · nothing changed";
    si.hint = "Press: try again  ·  hold 3 s: re-check";
    shot(cv, "13-system-failed", [&](Adafruit_GFX &d) { renderSystem(d, si, sc); });
  }

  // 6. Setup screen.
  SetupInfo si;
  si.apName = "Planula-6D09";
  si.apPass = "reef-7k2m";
  si.url = "http://192.168.4.1";
  si.reason = "No Wi-Fi saved yet.";
  Chrome sc;
  PANEL_COPY(sc.footerLeft, "planula-epaper-1 · setup mode");
  PANEL_COPY(sc.footerRight, "build 12");
  shot(cv, "6-setup", [&](Adafruit_GFX &d) { renderSetup(d, si, sc); });

  // 7. Stale footer + waiting message.
  Chrome wc;
  PANEL_COPY(wc.footerLeft, "STALE · no page from Coeno for 7 min");
  PANEL_COPY(wc.footerRight, "Wi-Fi -61 dBm");
  wc.footerAlert = true;
  PANEL_COPY(wc.clock, "8:41");
  shot(cv, "7-waiting", [&](Adafruit_GFX &d) { renderMessage(d, "Waiting for Coeno", "Connected to Wi-Fi as 10.0.0.87.\nCoeno at 10.0.0.228 has not sent a page yet.\nSettings: http://planula-epaper-1.local", "10.0.0.87", wc); });
}

static void tests() {
  char b[64];
  toGlyphs("25.9 °C · CO₂ µS", b, sizeof b);
  CHECK(strchr(b, 0x7F) != nullptr);
  CHECK(strchr(b, (char)0x82) != nullptr);
  CHECK(strchr(b, (char)0x80) != nullptr);

  Gauge g;
  g.has = true;
  g.value = -0.001f;
  g.dec = 2;
  formatValue(g, b, sizeof b);
  CHECK(!strcmp(b, "0.00"));
  g.has = false;
  formatValue(g, b, sizeof b);
  CHECK(!strcmp(b, "--"));
  formatAge(5, b, sizeof b); CHECK(!strcmp(b, "now"));
  formatAge(75, b, sizeof b); CHECK(!strcmp(b, "75 s"));
  formatAge(14 * 60, b, sizeof b); CHECK(!strcmp(b, "14 min"));
  formatAge(5 * 3600, b, sizeof b); CHECK(!strcmp(b, "5 h"));

  Limits L{7.9f, 8.4f, 7.6f, 8.6f};
  Gauge e;
  e.has = true;
  e.value = 8.1f;
  evaluate(e, L, 600); CHECK(e.status == Status::Ok); CHECK(e.hasRange);
  e.value = 8.45f; evaluate(e, L, 600); CHECK(e.status == Status::Warn); CHECK(!strcmp(e.note, "HIGH"));
  e.value = 7.5f; evaluate(e, L, 600); CHECK(e.status == Status::Alarm); CHECK(!strcmp(e.note, "LOW"));
  e.age_s = 700; evaluate(e, L, 600); CHECK(e.status == Status::Stale);
  e.has = false; evaluate(e, L, 600); CHECK(e.status == Status::Missing);
  Limits none{NAN, NAN, NAN, NAN};
  e.has = true; e.age_s = 0; e.value = 999; evaluate(e, none, 600); CHECK(e.status == Status::Ok); CHECK(!e.hasRange);

  const char *js = R"({"v":1,"title":"Tank","layout":"3x2","gauges":[
    {"id":"tank:ph","label":"pH","value":8.12,"dec":2,"unit":"","src":"Apex","status":"warn","note":"HIGH","age_s":30,"lo":7.9,"hi":8.4,"spark":[8.1,8.11,8.12]},
    {"id":"x","label":"Nothing","value":null},
    {"id":"y","label":"Int","value":450,"dec":0}]})";
  Page p;
  CHECK(!parsePage(js, strlen(js), p)[0]);
  CHECK(p.n == 3);
  CHECK(p.layout == Layout::Grid3x2);
  CHECK(p.g[0].status == Status::Warn && p.g[0].nspark == 3 && p.g[0].hasRange && fabsf(p.g[0].value - 8.12f) < 1e-4);
  CHECK(p.g[1].status == Status::Missing && !p.g[1].has);
  CHECK(p.g[2].has && p.g[2].status == Status::Ok && p.g[2].value == 450);
  CHECK(!p.g[0].hasSparkRange);
  CHECK(parsePage("nope", 4, p)[0]);

  // spark_lo / spark_hi: optional, both numbers, hi > lo.
  {
    const char *js = R"({"v":1,"gauges":[
      {"id":"a","value":8.1,"spark":[8.1,8.11],"spark_lo":7.9,"spark_hi":8.3},
      {"id":"b","value":8.1,"spark_lo":8,"spark_hi":8},
      {"id":"c","value":8.1,"spark_lo":7.9},
      {"id":"d","value":8.1,"spark_lo":"7.9","spark_hi":8.3}]})";
    Page sp;
    CHECK(!parsePage(js, strlen(js), sp)[0]);
    CHECK(sp.n == 4);
    CHECK(sp.g[0].hasSparkRange && fabsf(sp.g[0].sparkLo - 7.9f) < 1e-4 && fabsf(sp.g[0].sparkHi - 8.3f) < 1e-4);
    CHECK(!sp.g[1].hasSparkRange && !sp.g[2].hasSparkRange && !sp.g[3].hasSparkRange);
  }

  int c, r;
  Page q; q.n = 5; gridFor(q, c, r); CHECK(c == 3 && r == 2);
  q.n = 9; gridFor(q, c, r); CHECK(c == 0);
  q.n = 2; gridFor(q, c, r); CHECK(c == 2 && r == 1);

  // Timeline lanes.
  {
    const char *js = R"({"layout":"timeline","window":"day","clock":600,"gauges":[
      {"id":"l","kind":"level","pts":[[60,100],[-60,0],[0,50]]},
      {"id":"s","kind":"step","pts":[[-60,0],[0,50],[60,100]]},
      {"id":"o","spans":[[-30,30]]},
      {"id":"e","kind":"dose","pts":[[-5,2.5],[5]]}]})";
    Page t;
    CHECK(!parsePage(js, strlen(js), t)[0]);
    CHECK(t.layout == Layout::Timeline && t.n == 4);
    CHECK(t.from == -600 && t.to == 840 && t.clock == 600);
    CHECK(t.g[0].t[0] == -60 && t.g[0].t[2] == 60);  // sorted
    CHECK(fabsf(laneValueAt(t.g[0], -30) - 25) < 1e-3);
    CHECK(fabsf(laneValueAt(t.g[1], -30) - 0) < 1e-3);
    CHECK(isnan(laneValueAt(t.g[0], -61)));
    CHECK(t.g[2].lane == Lane::OnOff && t.g[2].nspark == 2 && laneValueAt(t.g[2], 0) == 1 && laneValueAt(t.g[2], 31) == 0);
    CHECK(t.g[3].lane == Lane::Events && t.g[3].nspark == 2 && isnan(t.g[3].spark[1]));
    CHECK(t.g[0].status == Status::Ok);
    shiftTimeline(t, 30);
    CHECK(t.clock == 630 && t.from == -630 && t.to == 810 && t.g[0].t[0] == -90);
  }

  // SigV4: AWS's published S3 presigned-URL example.
  sigv4::Presign sp;
  sp.host = "examplebucket.s3.amazonaws.com"; sp.path = "/test.txt"; sp.service = "s3"; sp.region = "us-east-1";
  sp.accessKey = "AKIAIOSFODNN7EXAMPLE"; sp.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
  sp.amzDate = "20130524T000000Z"; sp.expires = 86400; sp.payloadHash = "UNSIGNED-PAYLOAD";
  CHECK(sigv4::presignQuery(sp).find("X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404") != std::string::npos);
  std::string iot = sigv4::iotWebsocketPath("abc-ats.iot.us-west-2.amazonaws.com", "us-west-2", "AK", "SK", "tok/en+", "20261029T120000Z");
  CHECK(iot.rfind("/mqtt?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AK%2F20261029%2Fus-west-2%2Fiotdevicegateway", 0) == 0);
  CHECK(iot.find("&X-Amz-Security-Token=tok%2Fen%2B") != std::string::npos);
  CHECK(iot.find("X-Amz-Security-Token") > iot.find("X-Amz-Signature"));

  testRules();
  printf(failures ? "%d test(s) FAILED\n" : "all tests passed\n", failures);
}


// ── rules: battery redraws, wake times, outlets, controller states ─────────
static Gauge gauge(const char *label, const char *unit, float v, uint8_t dec, Status st = Status::Ok) {
  Gauge g;
  PANEL_COPY(g.label, label);
  PANEL_COPY(g.unit, unit);
  g.has = true;
  g.value = v;
  g.dec = dec;
  g.status = st;
  return g;
}

static void testHistory() {
  using namespace rules;
  // Apex log dates: epoch numbers (AOS 5.x) and classic local YYYYMMDDHHMM strings
  CHECK(apexDate(1762525579.0, nullptr) == 1762525579u);
  CHECK(apexDate(0, "1762525579") == 1762525579u);
  CHECK(apexDate(0, "") == 0);
  CHECK(apexDate(0, "garbage") == 0);
  {
    struct tm tm = {};
    tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 7; tm.tm_hour = 9; tm.tm_min = 26; tm.tm_isdst = -1;
    CHECK(apexDate(0, "202611070926") == (uint32_t)mktime(&tm));
  }
  // slots: 48 x 30 min ending now
  uint32_t now = 1762525579u;
  CHECK(slotFor(now, now, 48, 1800) == 47);
  CHECK(slotFor(now - 1799, now, 48, 1800) == 47);
  CHECK(slotFor(now - 1800, now, 48, 1800) == 46);
  CHECK(slotFor(now - 48 * 1800 + 1, now, 48, 1800) == 0);
  CHECK(slotFor(now - 48 * 1800, now, 48, 1800) == -1);  // older than the day
  CHECK(slotFor(now + 60, now, 48, 1800) == 47);          // Apex clock a minute ahead
  CHECK(slotFor(now + 3600, now, 48, 1800) == -1);        // an hour ahead: wrong clock
  // slot means -> points: leading gaps dropped, later gaps carried forward
  float sum[6] = {0, 0, 16.2f, 0, 8.0f, 24.6f};
  uint16_t cnt[6] = {0, 0, 2, 0, 1, 3};
  float pts[6];
  int np = slotPoints(sum, cnt, 6, pts);
  CHECK(np == 4);
  CHECK(fabsf(pts[0] - 8.1f) < 1e-4f && fabsf(pts[1] - 8.1f) < 1e-4f);
  CHECK(fabsf(pts[2] - 8.0f) < 1e-4f && fabsf(pts[3] - 8.2f) < 1e-4f);
  uint16_t none[3] = {0, 0, 0};
  CHECK(slotPoints(sum, none, 3, pts) == 0);
  // doses from a dosing log carry their amount to the lane
  {
    const uint32_t H = 3600000u;
    OutletHistory d = {};
    d.recordDose(2 * H, 2.5f);
    d.recordDose(6 * H, 3.0f);
    d.recordDose(7 * H, 0);  // amount unknown
    CHECK(d.len == 6);
    int16_t t[48];
    float v[48];
    int n = lanePoints(d, 8 * H, -720, true, false, t, v, 48);
    CHECK(n == 3);
    CHECK(t[0] == -360 && fabsf(v[0] - 2.5f) < 1e-4f);
    CHECK(t[1] == -120 && fabsf(v[1] - 3.0f) < 1e-4f);
    CHECK(t[2] == -60 && isnan(v[2]));
    d.record(8 * H, 100);  // a live "running" report after the log: a dose start without amount
    n = lanePoints(d, 8 * H, -720, true, false, t, v, 48);
    CHECK(n == 4 && t[3] == 0 && isnan(v[3]));
  }
  // output log states
  CHECK(apexOn("ON") && apexOn("AON") && apexOn("TBL") && apexOn("1"));
  CHECK(!apexOn("OFF") && !apexOn("AOF") && !apexOn("0") && !apexOn(nullptr));
}

static void testRules() {
  using namespace rules;
  testHistory();
  // signed elapsed time: a stamp a little newer than "now" is not 49 days old
  CHECK(since(1000, 1500) < 0);
  CHECK(since(5000, 1000) == 4000);
  CHECK(since(100, 0xFFFFFF00u) == 356);  // across the millis() wrap

  // redraw steps
  CHECK(fabsf(redrawStep(gauge("1R pH", "", 8.1f, 2)) - 0.02f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("Sump PH", "", 8.1f, 2)) - 0.02f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("Temp", "°F", 77.8f, 1)) - 0.2f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("Temp", "°C", 25.9f, 1)) - 0.1f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("Salinity", "ppt", 33.0f, 1)) - 0.1f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("ORP", "mV", 312, 0)) - 5.0f) < 1e-6f);
  CHECK(fabsf(redrawStep(gauge("Calcium", "ppm", 420, 0)) - 4.2f) < 1e-4f);

  // compare: under the step = no move; a status change; a new page
  Page p;
  p.n = 2;
  p.g[0] = gauge("1R pH", "", 8.10f, 2);
  p.g[1] = gauge("Salinity", "ppt", 33.0f, 1);
  Drawn d = {};
  remember(p, 0, d);
  bool moved, st;
  p.g[0].value = 8.11f;
  compare(p, 0, d, moved, st);
  CHECK(!moved && !st);
  p.g[0].value = 8.12f;
  compare(p, 0, d, moved, st);
  CHECK(moved && !st);
  p.g[0].value = 8.10f;
  p.g[1].status = Status::Warn;
  compare(p, 0, d, moved, st);
  CHECK(!moved && st);
  compare(p, 1, d, moved, st);
  CHECK(moved && st);
  p.g[1].status = Status::Ok;
  p.g[1].has = false;
  compare(p, 0, d, moved, st);
  CHECK(moved);

  // which wakes redraw
  CHECK(shouldRedraw(Wake::Daily, false, false, 10));
  CHECK(!shouldRedraw(Wake::Hourly, true, false, 7200));
  CHECK(shouldRedraw(Wake::Hourly, false, true, 10));
  CHECK(!shouldRedraw(Wake::Saver, false, false, 600));
  CHECK(shouldRedraw(Wake::Saver, false, false, 3600));
  CHECK(shouldRedraw(Wake::Saver, true, false, 60));

  // wake times in Detroit, across midnight and the DST change (Nov 1 2026)
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();
  auto at = [](int y, int mo, int dd, int h, int mi) {
    struct tm tm = {};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = dd; tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
    return mktime(&tm);
  };
  auto hm = [](time_t t, int &h, int &m, int &day) {
    struct tm tm;
    localtime_r(&t, &tm);
    h = tm.tm_hour; m = tm.tm_min; day = tm.tm_mday;
  };
  int h, m, day;
  hm(nextAt(at(2026, 11, 6, 22, 0), 7, 0), h, m, day);
  CHECK(h == 7 && m == 0 && day == 7);
  hm(nextAt(at(2026, 11, 7, 6, 0), 7, 0), h, m, day);
  CHECK(h == 7 && m == 0 && day == 7);
  hm(nextAt(at(2026, 10, 31, 20, 0), 7, 0), h, m, day);  // the night the clocks go back
  CHECK(h == 7 && m == 0 && day == 1);
  Wake why;
  time_t now = at(2026, 11, 7, 3, 40);
  time_t w = nextWake(now, true, 'd', 10, 7, 0, true, why);
  hm(w, h, m, day);
  CHECK(why == Wake::Hourly && h == 4 && m == 0);
  w = nextWake(at(2026, 11, 7, 6, 59), true, 'd', 10, 7, 0, true, why);
  hm(w, h, m, day);
  CHECK(why == Wake::Daily && h == 7 && m == 0);
  w = nextWake(now, true, 'd', 10, 7, 0, false, why);
  CHECK(why == Wake::Daily);
  w = nextWake(now, true, 's', 10, 7, 0, true, why);
  CHECK(why == Wake::Saver && w - now == 600);
  w = nextWake(now, false, 'd', 10, 7, 0, true, why);
  CHECK(why == Wake::Saver && w - now == 3600);

  // outlet history: small ramp steps dropped, on/off always kept
  OutletHistory oh = {};
  oh.record(0, 0);
  oh.record(1000, 2);   // to on: kept
  oh.record(2000, 4);   // +2: dropped
  oh.record(3000, 9);   // +7 from 2: kept
  oh.record(4000, 0);   // off: kept
  oh.record(5000, 0);   // same: dropped
  CHECK(oh.len == 4);
  for (int i = 0; i < 60; i++) oh.record(6000 + i * 1000, (i % 2) ? 100 : 0);
  CHECK(oh.len == kHist);  // full, oldest dropped

  // lanes over a 12 h window: state at the window start, then changes
  OutletHistory heater = {};
  uint32_t H = 3600000u, nowMs = 20 * H;
  heater.record(1 * H, 100);   // 19 h ago: before the window
  heater.record(10 * H, 0);    // 10 h ago
  heater.record(15 * H, 100);  // 5 h ago
  int16_t t[48];
  float v[48];
  int n = lanePoints(heater, nowMs, -720, false, false, t, v, 48);
  CHECK(n == 3 && t[0] == -720 && v[0] == 1 && t[1] == -600 && v[1] == 0 && t[2] == -300 && v[2] == 1);
  // nothing inside the window: just the current state from the start
  OutletHistory old = {};
  old.record(1 * H, 100);
  n = lanePoints(old, nowMs, -720, false, false, t, v, 48);
  CHECK(n == 1 && t[0] == -720 && v[0] == 1);
  // dosers: one mark per dose start inside the window
  OutletHistory dose = {};
  dose.record(5 * H, 100);
  dose.record(5 * H + 30000, 0);
  dose.record(12 * H, 100);
  dose.record(12 * H + 30000, 0);
  dose.record(18 * H, 100);
  n = lanePoints(dose, nowMs, -720, true, false, t, v, 48);
  CHECK(n == 2 && t[0] == -480 && t[1] == -120 && isnan(v[0]));
  // a wrapped millis() clock still gives the right ages
  OutletHistory wrap = {};
  wrap.record(0xFFFFFFFFu - 600000u, 100);  // 10 min before the wrap
  n = lanePoints(wrap, 600000u, -720, false, false, t, v, 48);
  CHECK(n == 1 && t[0] == -20);

  // Hydros Output entries (as logged from Joe's Hydros, 2026-11-06)
  CHECK(hydrosLevel(10000, NAN, NAN, false) == 100);  // 1RTunzeSmp
  CHECK(hydrosLevel(0, NAN, NAN, true) == 0);         // 1RX10D1-Part C idle
  CHECK(hydrosLevel(2500, NAN, NAN, true) == 100);    // doser running: on/off
  CHECK(hydrosLevel(4250, NAN, NAN, false) == 43);    // dimmed light
  CHECK(hydrosLevel(30, NAN, NAN, false) == 1);       // barely on is still on
  CHECK(hydrosLevel(0, 0, 0, false) == 0);            // 1R WATERCHANGE idle
  CHECK(hydrosLevel(0, 0, 10000, false) == 100);      // filling
  CHECK(hydrosLevel(NAN, NAN, NAN, false) == 0);

  // Apex output status
  bool on, man, hid;
  apexOutput("AON", "outlet", on, man, hid);
  CHECK(on && !man && !hid);
  apexOutput("OFF", "outlet", on, man, hid);
  CHECK(!on && man);
  apexOutput("ON", "24v", on, man, hid);
  CHECK(on && man);
  apexOutput("AOF", "virtual", on, man, hid);
  CHECK(!on && hid);
  apexOutput("TBL", "variable", on, man, hid);
  CHECK(on && !man);
}

int main(int argc, char **argv) {
  if (argc < 2 || !strcmp(argv[1], "render")) samples();
  tests();
  return failures ? 1 : 0;
}
