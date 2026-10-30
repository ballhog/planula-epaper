// Host build of lib/panel: renders sample screens to PBM (render.sh turns them
// into PNG) and runs the pure-logic tests. Same drawing code as the panel.
//
//   tools/host/render.sh          → tools/host/out/*.png, then runs the tests
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_GFX.h>

#include <functional>

#include "BandCanvas.h"
#include "Model.h"
#include "PageJson.h"
#include "Render.h"
#include "SigV4.h"
#include "Text.h"

using namespace panel;

static int failures = 0;
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
  PANEL_COPY(c.footerLeft, "Coeno · page updated 40 s ago");
  PANEL_COPY(c.footerRight, "Wi-Fi -58 dBm · build 12");
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
    PANEL_COPY(sc.footerRight, "Wi-Fi -58 dBm · update ready");
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
    PANEL_COPY(sc.footerRight, "Wi-Fi -58 dBm");
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
  CHECK(parsePage("nope", 4, p)[0]);

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

  printf(failures ? "%d test(s) FAILED\n" : "all tests passed\n", failures);
}

int main(int argc, char **argv) {
  if (argc < 2 || !strcmp(argv[1], "render")) samples();
  tests();
  return failures ? 1 : 0;
}
