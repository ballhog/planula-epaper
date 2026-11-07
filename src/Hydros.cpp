#include "Hydros.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <mqtt_client.h>
#include <time.h>

#include <utility>
#include <vector>

#include "Https.h"
#include "Readings.h"
#include "Settings.h"
#include "SigV4.h"
#include "WsTransport.h"
#include "Worker.h"
#include "Outlets.h"
#include "Rules.h"
#include "miniz.h"

namespace hydros {

namespace {

const char *kEndpointKeys[] = {"iotEndpoint", "iot_endpoint", "mqttEndpoint", "mqtt_endpoint", "awsIotEndpoint",
                               "aws_iot_endpoint", "iotEndpointAddress"};
const char *kClientId = "3au5f4m5juu58qks62mcd89730";  // Hydros app (from pyhydros)
const char *kApi = "https://cv.hydros.link";
const uint32_t kRebuildMs = 50UL * 60UL * 1000UL;
const uint32_t kSilentMs = 10UL * 60UL * 1000UL;

struct Thing {
  String id, name;
};

esp_mqtt_client_handle_t client = nullptr;
volatile bool connected = false;
volatile uint32_t lastMsg = 0;
volatile uint32_t connectedAt = 0;
bool onlyFirst = false;
// The AWS session holds the TLS token (see Worker.h) from login until stopClient().
bool holdsTls = false;
// Login runs on the worker; the loop picks the result up. gen discards a login
// that finished after restart() (settings changed meanwhile).
uint8_t gen = 0;
volatile bool loginDone = false;
uint8_t loginGen = 0;
String loginErr;
uint32_t sessionStart = 0, nextTry = 0, backoff = 15000;
String state = "off", userId;
String lastLogged;
// Session built from saved credentials: if it never connects they're dropped.
bool fromCache = false;
volatile bool sawConnect = false;
time_t credExp = 0;  // when the session's AWS credentials run out (0 unknown)
// The Cognito id token (an hour), for the REST calls made while the session
// holds the TLS token (the dosing log). Saved with the sign-in.
String idTok;
uint32_t idTokExp = 0;
RTC_DATA_ATTR uint32_t doseLogsAtS;  // last dosing-log read (epoch s), kept through sleep

void setState(const String &st) {
  state = st;
  if (st != lastLogged && !st.startsWith("ok")) {
    Serial.printf("[hydros] %s\n", st.c_str());
    lastLogged = st;
  } else if (st.startsWith("ok") && !lastLogged.startsWith("ok")) {
    Serial.printf("[hydros] %s\n", st.c_str());
    lastLogged = st;
  }
}
std::vector<Thing> things;
String wsHost, wsPath, clientIdStr;

// Reassembly of one MQTT message that arrives in chunks.
char *rx = nullptr;
size_t rxLen = 0, rxCap = 0;
String rxTopic;

String jwtField(const String &jwt, const char *field) {
  int a = jwt.indexOf('.'), b = jwt.indexOf('.', a + 1);
  if (a < 0 || b < 0) return "";
  String p = jwt.substring(a + 1, b);
  p.replace('-', '+');
  p.replace('_', '/');
  while (p.length() % 4) p += '=';
  size_t n = 0;
  std::vector<uint8_t> out(p.length());
  if (mbedtls_base64_decode(out.data(), out.size(), &n, (const uint8_t *)p.c_str(), p.length())) return "";
  JsonDocument d;
  if (deserializeJson(d, (const char *)out.data(), n)) return "";
  return d[field] | "";
}

String amzDate() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char b[20];
  strftime(b, sizeof b, "%Y%m%dT%H%M%SZ", &t);
  return b;
}

String findEndpoint(JsonVariantConst v) {
  for (const char *k : kEndpointKeys)
    if (v[k].is<const char *>()) return v[k].as<const char *>();
  for (JsonPairConst kv : v.as<JsonObjectConst>())
    if (kv.value().is<JsonObjectConst>())
      for (const char *k : kEndpointKeys)
        if (kv.value()[k].is<const char *>()) return kv.value()[k].as<const char *>();
  return "";
}

struct Login {
  String idToken, region = "us-west-2", pool, error;
};

Login login(const String &user, const String &pass) {
  Login L;
  JsonDocument b;
  b["AuthFlow"] = "USER_PASSWORD_AUTH";
  b["ClientId"] = kClientId;
  b["AuthParameters"]["USERNAME"] = user;
  b["AuthParameters"]["PASSWORD"] = pass;
  String body;
  serializeJson(b, body);
  https::Result r = https::request("POST", "https://cognito-idp." + L.region + ".amazonaws.com/",
                                   {{"Content-Type", "application/x-amz-json-1.1"},
                                    {"X-Amz-Target", "AWSCognitoIdentityProviderService.InitiateAuth"}},
                                   body, 16384);
  if (r.status < 0) { L.error = "can't reach Hydros login: " + r.error; return L; }
  JsonDocument d;
  deserializeJson(d, r.body);
  if (r.status != 200) {
    String m = d["message"] | "";
    L.error = "login refused" + (m.length() ? ": " + m : String(" (HTTP ") + r.status + ")");
    return L;
  }
  L.idToken = d["AuthenticationResult"]["IdToken"] | "";
  if (!L.idToken.length()) {
    String ch = d["ChallengeName"] | "";
    L.error = ch.length() ? "login needs " + ch + " (finish it in the Hydros app)" : "no token in reply";
    return L;
  }
  String iss = jwtField(L.idToken, "iss");  // https://cognito-idp.<region>.amazonaws.com/<pool>
  int slash = iss.lastIndexOf('/');
  L.pool = slash >= 0 ? iss.substring(slash + 1) : "";
  int us = L.pool.indexOf('_');
  if (us > 0) L.region = L.pool.substring(0, us);
  return L;
}

// Only what the panel uses survives the download: the profile and thing
// replies can be large (every controller's whole setup).
void endpointFilter(JsonObject f) {
  for (const char *k : kEndpointKeys) {
    f[k] = true;
    f["*"][k] = true;  // one level down, like pyhydros looks
  }
}

https::Result api(const String &path, const String &idToken, JsonDocument &out, bool profile) {
  JsonDocument filter;
  JsonObject f = filter.to<JsonObject>();
  endpointFilter(f);
  if (profile) {
    f["generated_user_id"] = true;
    JsonObject t = f["things"][0].to<JsonObject>();
    for (const char *k : {"thingName", "id", "thingId", "friendlyName", "name", "thingType", "parent", "parentThing"})
      t[k] = true;
  } else {
    f["cognito_identity"] = true;
    f["parent"] = true;
  }
  return https::requestJson("GET", String(kApi) + path,
                            {{"Authorization", idToken}, {"Content-Type", "application/json"}}, "", out, &filter);
}

// --- Saved sign-in -----------------------------------------------------------
// Every battery wake used to sign in from scratch: Cognito login, /user,
// /thing, GetCredentialsForIdentity, then the IoT socket (four or five TLS
// handshakes, most of the awake time). The account details (user id,
// controllers, identity, endpoint) barely change and the AWS credentials are
// good for an hour, so both are kept in flash (NVS: RTC memory is too small
// for the ~1.5 kB session token) and reused:
//   credentials still good   -> straight to the IoT socket (no HTTPS at all)
//   credentials ran out      -> login + credentials only (skips /user, /thing)
//   nothing saved / > 1 day  -> the full sign-in
// Anything that fails with saved data drops it and does the full sign-in.
struct Saved {
  String uid, identity, endpoint, region, things;  // things: "id\tname\n"...
  uint32_t at = 0;                                 // profile saved (epoch s)
  String ak, sk, tok;
  uint32_t exp = 0;                                // credentials expire (epoch s)
  String idt;                                      // Cognito id token
  uint32_t idtExp = 0;
};
const char *kNvs = "hydros";

bool loadSaved(const String &user, Saved &c) {
  Preferences p;
  if (!p.begin(kNvs, true)) return false;
  bool ok = p.getString("user", "") == user;
  if (ok) {
    c.uid = p.getString("uid", "");
    c.identity = p.getString("ident", "");
    c.endpoint = p.getString("ep", "");
    c.region = p.getString("reg", "");
    c.things = p.getString("things", "");
    c.at = p.getUInt("at", 0);
    c.ak = p.getString("ak", "");
    c.sk = p.getString("sk", "");
    c.tok = p.getString("tok", "");
    c.exp = p.getUInt("exp", 0);
    c.idt = p.getString("idt", "");
    c.idtExp = p.getUInt("idte", 0);
  }
  p.end();
  return ok && c.uid.length() && c.identity.length() && c.endpoint.length() && c.things.length();
}

void saveProfile(const String &user, const Saved &c) {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.putString("user", user);
  p.putString("uid", c.uid);
  p.putString("ident", c.identity);
  p.putString("ep", c.endpoint);
  p.putString("reg", c.region);
  p.putString("things", c.things);
  p.putUInt("at", c.at);
  p.end();
}

void saveCreds(const Saved &c) {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  bool fits = c.tok.length() < 3900;  // NVS strings top out at 4000 bytes
  p.putString("ak", fits ? c.ak : String());
  p.putString("sk", fits ? c.sk : String());
  p.putString("tok", fits ? c.tok : String());
  p.putUInt("exp", fits ? c.exp : 0);
  bool idFits = c.idt.length() < 3900;
  p.putString("idt", idFits ? c.idt : String());
  p.putUInt("idte", idFits ? c.idtExp : 0);
  p.end();
}

void forgetCreds() {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.putUInt("exp", 0);
  p.end();
}

void forgetAll() {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.clear();
  p.end();
}

String thingsText(const std::vector<Thing> &v) {
  String o;
  for (auto &t : v) o += t.id + "\t" + t.name + "\n";
  return o;
}

std::vector<Thing> thingsFrom(const String &txt) {
  std::vector<Thing> v;
  int a = 0;
  while (a < (int)txt.length()) {
    int nl = txt.indexOf('\n', a);
    if (nl < 0) nl = txt.length();
    int tab = txt.indexOf('\t', a);
    if (tab > a && tab < nl) v.push_back({txt.substring(a, tab), txt.substring(tab + 1, nl)});
    a = nl + 1;
  }
  return v;
}

// AWS credentials for the controller's identity. Fills c.ak/sk/tok/exp.
String getCreds(const Login &L, Saved &c) {
  JsonDocument cb;
  cb["IdentityId"] = c.identity;
  cb["Logins"]["cognito-idp." + L.region + ".amazonaws.com/" + L.pool] = L.idToken;
  String body;
  serializeJson(cb, body);
  https::Result r = https::request("POST", "https://cognito-identity." + c.region + ".amazonaws.com/",
                                   {{"Content-Type", "application/x-amz-json-1.1"},
                                    {"X-Amz-Target", "AWSCognitoIdentityService.GetCredentialsForIdentity"}},
                                   body, 16384);
  if (r.status != 200) return "AWS credentials refused (HTTP " + String(r.status) + ")";
  JsonDocument cd;
  deserializeJson(cd, r.body);
  c.ak = cd["Credentials"]["AccessKeyId"] | "";
  c.sk = cd["Credentials"]["SecretKey"] | "";
  c.tok = cd["Credentials"]["SessionToken"] | "";
  double exp = cd["Credentials"]["Expiration"] | 0.0;  // epoch seconds
  c.exp = exp > 1700000000 ? (uint32_t)exp : (uint32_t)time(nullptr) + 50 * 60;
  if (!c.ak.length() || !c.sk.length()) return "no AWS credentials in reply";
  return "";
}

// The signed IoT WebSocket URL, from saved or fresh credentials.
void useSaved(const Saved &c) {
  userId = c.uid;
  things = thingsFrom(c.things);
  wsHost = c.endpoint;
  wsPath = sigv4::iotWebsocketPath(c.endpoint.c_str(), c.region.c_str(), c.ak.c_str(), c.sk.c_str(), c.tok.c_str(),
                                   amzDate().c_str())
               .c_str();
  credExp = c.exp;
  idTok = c.idt;
  idTokExp = c.idtExp;
}

// Steps 1-4. On success fills things, userId, wsHost, wsPath.
String connectSteps(const String &user, const String &pass, bool forTest, String *summary) {
  if (!WiFi.isConnected()) return "no Wi-Fi";
  if (time(nullptr) < 1700000000) return "waiting for the clock (NTP)";
  fromCache = false;
  Saved c;
  uint32_t nowS = (uint32_t)time(nullptr);
  bool saved = !forTest && loadSaved(user, c) && nowS - c.at < 24UL * 3600UL;
  if (saved && c.ak.length() && c.exp > nowS + 600) {
    useSaved(c);
    fromCache = true;
    Serial.printf("[hydros] saved sign-in (AWS credentials good for %u more min)\n", (unsigned)((c.exp - nowS) / 60));
    return "";
  }
  Login L = login(user, pass);
  if (L.error.length()) return L.error;
  if (saved) {
    c.idt = L.idToken;
    c.idtExp = (uint32_t)time(nullptr) + 55 * 60;  // Cognito id tokens last an hour
    String e = getCreds(L, c);
    if (!e.length()) {
      useSaved(c);
      saveCreds(c);
      fromCache = true;
      Serial.printf("[hydros] signed in with the saved account details (new AWS credentials)\n");
      return "";
    }
    Serial.printf("[hydros] saved account details didn't work (%s); full sign-in\n", e.c_str());
    forgetAll();
  }

  JsonDocument ud;
  https::Result u = api("/user", L.idToken, ud, true);
  if (u.status < 0) return "Hydros /user: " + u.error;
  if (u.status != 200) return "Hydros /user failed (HTTP " + String(u.status) + ")";
  if (u.error.length()) return "Hydros /user: bad reply (" + u.error + ")";
  String uid = ud["generated_user_id"] | "";
  if (!uid.length()) return "Hydros /user: no user id";
  String endpoint = findEndpoint(ud.as<JsonVariantConst>());

  std::vector<Thing> all, top;
  for (JsonObjectConst t : ud["things"].as<JsonArrayConst>()) {
    Thing th;
    th.id = t["thingName"] | (t["id"] | (t["thingId"] | ""));
    if (!th.id.length()) continue;
    th.name = t["friendlyName"] | (t["name"] | th.id.c_str());
    all.push_back(th);
    bool parent = t["parent"].is<const char *>() || t["parentThing"].is<const char *>();
    String type = t["thingType"] | "";
    if (type == "Collective" || !parent) top.push_back(th);
  }
  ud.clear();
  if (top.empty()) top = all;
  if (top.empty()) return "no Hydros controllers on this account";
  if (summary) {
    *summary = String(top.size()) + " controller" + (top.size() == 1 ? "" : "s") + ": ";
    for (size_t i = 0; i < top.size(); i++) *summary += (i ? ", " : "") + top[i].name;
  }
  // The test button stops here and leaves the live session's state alone.
  if (forTest) return "";
  things = top;  // only touched while no MQTT client exists (loop() calls this between sessions)
  userId = uid;

  JsonDocument td;
  https::Result t = api(String("/thing/") + sigv4::uriEncode(top[0].id.c_str()).c_str(), L.idToken, td, false);
  if (t.status != 200) return "Hydros /thing failed (HTTP " + String(t.status) + ")";
  if (t.error.length()) return "Hydros /thing: bad reply (" + t.error + ")";
  String identity = td["cognito_identity"] | "";
  String parent = td["parent"] | "";
  if (!endpoint.length()) endpoint = findEndpoint(td.as<JsonVariantConst>());
  if (!identity.length() && parent.length()) {
    JsonDocument pd;
    api("/thing/" + parent, L.idToken, pd, false);
    identity = pd["cognito_identity"] | "";
    if (!endpoint.length()) endpoint = findEndpoint(pd.as<JsonVariantConst>());
  }
  if (!identity.length()) return "no cognito identity for the controller";
  if (!endpoint.length()) return "Hydros didn't say which AWS IoT endpoint to use";
  endpoint.replace("https://", "");
  endpoint.replace("wss://", "");
  int sl = endpoint.indexOf('/');
  if (sl > 0) endpoint = endpoint.substring(0, sl);
  // Region of the IoT endpoint (xxxx-ats.iot.<region>.amazonaws.com) wins.
  String region = L.region;
  int iot = endpoint.indexOf(".iot.");
  if (iot > 0) {
    int dot = endpoint.indexOf('.', iot + 5);
    if (dot > 0) region = endpoint.substring(iot + 5, dot);
  }

  Saved n;
  n.uid = uid;
  n.identity = identity;
  n.endpoint = endpoint;
  n.region = region;
  n.things = thingsText(top);
  n.at = (uint32_t)time(nullptr);
  n.idt = L.idToken;
  n.idtExp = n.at + 55 * 60;
  String e = getCreds(L, n);
  if (e.length()) return e;
  useSaved(n);
  saveProfile(user, n);
  saveCreds(n);
  return "";
}

float numberOf(JsonVariantConst v) {
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  return NAN;
}

// zlib inflate into a fixed buffer. Hydros status messages inflate to ~8.5 kB
// (measured on Joe's two controllers, 2026-10-29), so a 16 kB output buffer
// in non-wrapping mode does the job without the 32 kB sliding window; the
// decompressor (~11 kB) and buffer are allocated once at boot.
struct Inflater {
  static constexpr size_t kOut = 16 * 1024;
  tinfl_decompressor *d = nullptr;
  uint8_t *out = nullptr;
  bool ready() {
    if (!out) out = (uint8_t *)malloc(kOut);
    if (!d) d = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    return d && out;
  }
  // Returns the inflated length, or -1 (status in *st).
  long run(const uint8_t *in, size_t n, int *st) {
    tinfl_init(d);
    size_t inSize = n, outSize = kOut;
    tinfl_status s = tinfl_decompress(d, in, &inSize, out, out, &outSize,
                                      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    *st = (int)s;
    return s == TINFL_STATUS_DONE ? (long)outSize : -1;
  }
};
Inflater inflater;

void handlePayload(const String &topic, uint8_t *data, size_t len) {
  // "<user>/<thing>/rsp/LISTEN/statusc/<mac>"
  int a = topic.indexOf('/'), b = topic.indexOf('/', a + 1);
  String thing = (a >= 0 && b > a) ? topic.substring(a + 1, b) : "hydros";
  String device = thing;
  for (auto &t : things)
    if (t.id == thing) device = t.name;

  // Optional "<header> " in front of zlib data.
  uint8_t *p = data;
  size_t n = len;
  for (size_t i = 0; i + 2 < len && i < 64; i++)
    if (data[i] == ' ' && data[i + 1] == 0x78 && (data[i + 2] == 0x9C || data[i + 2] == 0x01 || data[i + 2] == 0xDA)) {
      p = data + i + 1;
      n = len - i - 1;
      break;
    }
  JsonDocument filter;
  filter["Input"]["*"]["probeValue"] = true;
  filter["Input"]["*"]["senseValue"] = true;
  filter["Input"]["*"]["i10Value"] = true;
  // Outputs (seen 2026-11-06): "Output":{"<name>":{"valueState":0..10000
  // (0-100 %), "override":bool, ...}}; dosing heads also carry "rpm", the
  // water-change pair "drainvalueState"/"fillvalueState".
  filter["Output"]["*"]["valueState"] = true;
  filter["Output"]["*"]["override"] = true;
  filter["Output"]["*"]["rpm"] = true;
  filter["Output"]["*"]["drainvalueState"] = true;
  filter["Output"]["*"]["fillvalueState"] = true;
  JsonDocument doc;
  DeserializationError err;
  if (n > 2 && p[0] == 0x78) {
    // Inflate as a stream through a fixed 32 kB window (allocated once), so the
    // size of the JSON inside doesn't matter. See InflateReader.
    if (!inflater.ready()) {
      Serial.printf("[hydros] no memory to inflate (largest free block %u bytes)\n", (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    int st = 0;
    long m = inflater.run(p, n, &st);
    if (m < 0) {
      Serial.printf("[hydros] %s: zlib failed (%d%s)\n", topic.c_str(), st,
                    st == TINFL_STATUS_HAS_MORE_OUTPUT ? ", inflates to more than 16 kB" : "");
      return;
    }
    err = deserializeJson(doc, (const char *)inflater.out, (size_t)m, DeserializationOption::Filter(filter));
  } else {
    err = deserializeJson(doc, (const char *)p, n, DeserializationOption::Filter(filter));
  }
  if (err) {
    Serial.printf("[hydros] %s: %u bytes, JSON %s (free %u)\n", topic.c_str(), (unsigned)len, err.c_str(),
                  (unsigned)ESP.getFreeHeap());
    return;
  }
  if (!lastMsg) Serial.printf("[hydros] first data %u s after start\n", (unsigned)(millis() / 1000));
  lastMsg = millis();
  static uint32_t lastLog = 0;
  if (!lastLog || millis() - lastLog > 300000UL) {
    lastLog = millis();
    Serial.printf("[hydros] data on %s: %u bytes, %u inputs\n", topic.c_str(), (unsigned)len,
                  (unsigned)doc["Input"].as<JsonObjectConst>().size());
  }
  for (JsonPairConst kv : doc["Input"].as<JsonObjectConst>()) {
    String name = kv.key().c_str();
    JsonObjectConst in = kv.value().as<JsonObjectConst>();
    String kind = readings::guessKind("", name);
    float v = numberOf(in["probeValue"]);
    if (isnan(v) && kind == "temp") v = numberOf(in["senseValue"]);
    if (isnan(v)) {
      v = numberOf(in["senseValue"]);  // switches, leak/level sensors: 0/1
      if (!isnan(v) && kind == "other") kind = "switch";
    }
    if (isnan(v)) continue;
    if (kind == "temp" && v > 50.0f) v = (v - 32.0f) * 5.0f / 9.0f;  // US accounts report °F
    readings::update("hydros:" + thing + "/" + name, "Hydros", device, name, kind, readings::unitFor(kind), v);
  }
  for (JsonPairConst kv : doc["Output"].as<JsonObjectConst>()) {
    JsonObjectConst o = kv.value().as<JsonObjectConst>();
    float v = numberOf(o["valueState"]);
    float dr = numberOf(o["drainvalueState"]), fi = numberOf(o["fillvalueState"]);
    if (isnan(v) && isnan(dr) && isnan(fi)) continue;
    bool doser = !o["rpm"].isNull(), wc = !isnan(dr) || !isnan(fi);
    uint8_t level = rules::hydrosLevel(v, dr, fi, doser);
    String key = "hydros:" + thing + "/" + kv.key().c_str();
    outlets::update(key.c_str(), "Hydros", doser ? "doser" : wc ? "wc" : "output", level, o["override"] | false, false);
  }
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
      sawConnect = true;
      lastMsg = millis();
      Serial.printf("[hydros] AWS IoT connected (%s), asking %u controller(s) for status\n", wsHost.c_str(),
                    (unsigned)things.size());
      connectedAt = millis();
      for (size_t i = 0; i < things.size(); i++) {
        if (onlyFirst && i > 0) break;
        auto &t = things[i];
        String rspT = userId + "/" + t.id + "/rsp/#";
        String reqT = userId + "/" + t.id + "/req/LISTEN/statusc";
        int sid = esp_mqtt_client_subscribe(e->client, rspT.c_str(), 1);
        int pid = esp_mqtt_client_publish(e->client, reqT.c_str(), "", 0, 1, 0);
        Serial.printf("[hydros] %s: subscribe %s (msg %d), request %s (msg %d)\n", t.name.c_str(), rspT.c_str(), sid,
                      reqT.c_str(), pid);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      Serial.println("[hydros] AWS IoT disconnected");
      if (!onlyFirst && things.size() > 1 && connectedAt && millis() - connectedAt < 15000) {
        onlyFirst = true;
        Serial.printf("[hydros] dropped right after asking %u controllers; next session asks only %s\n",
                      (unsigned)things.size(), things[0].name.c_str());
      }
      break;
    case MQTT_EVENT_ERROR:
      if (e->error_handle)
        Serial.printf("[hydros] AWS IoT error: type %d, tls 0x%x, errno %d, refused %d\n",
                      (int)e->error_handle->error_type, (unsigned)e->error_handle->esp_tls_last_esp_err,
                      e->error_handle->esp_transport_sock_errno, (int)e->error_handle->connect_return_code);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      // data = the SUBACK return code(s): 0..2 = granted QoS, 0x80 = refused by AWS's policy
      Serial.printf("[hydros] subscription msg %d: %s\n", e->msg_id,
                    (e->data_len > 0 && (uint8_t)e->data[0] == 0x80) ? "REFUSED by AWS" : "granted");
      break;
    case MQTT_EVENT_PUBLISHED:
      Serial.printf("[hydros] request msg %d delivered\n", e->msg_id);
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset == 0) {
        rxTopic = String(e->topic, e->topic_len);
        rxLen = 0;
        size_t need = e->total_data_len;
        static uint32_t msgs = 0;
        if (++msgs <= 3) Serial.printf("[hydros] message on %s, %u bytes\n", rxTopic.c_str(), (unsigned)need);
        if (need > 96 * 1024) {  // too big to hold: skip
          Serial.printf("[hydros] skipped %u-byte message on %s (too big)\n", (unsigned)need, rxTopic.c_str());
          rxCap = 0;
          free(rx);
          rx = nullptr;
          break;
        }
        if (need > rxCap) {
          free(rx);
          rx = (char *)malloc(need);
          rxCap = rx ? need : 0;
        }
      }
      if (!rx || e->current_data_offset + e->data_len > (int)rxCap) break;
      memcpy(rx + e->current_data_offset, e->data, e->data_len);
      rxLen = e->current_data_offset + e->data_len;
      if ((int)rxLen == e->total_data_len) handlePayload(rxTopic, (uint8_t *)rx, rxLen);
      break;
    }
    default:
      break;
  }
}

void stopClient() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  connected = false;
  free(rx);
  rx = nullptr;
  rxCap = rxLen = 0;
  if (holdsTls) {
    holdsTls = false;
    tls::release();
  }
}

bool startClient() {
  clientIdStr = "planula-" + String((uint32_t)ESP.getEfuseMac(), HEX) + "-" + String(esp_random() % 10000);
  esp_mqtt_client_config_t cfg = {};
  // Our own WebSocket transport: the framework's one can't send the ~1.9 kB
  // signed upgrade request (fixed 1 kB buffer). See WsTransport.h.
  esp_transport_handle_t tr = wst::create(wsPath.c_str(), "mqtt");
  if (!tr) return false;
  cfg.broker.address.hostname = wsHost.c_str();
  cfg.broker.address.port = 443;
  cfg.network.transport = tr;
  cfg.credentials.client_id = clientIdStr.c_str();
  cfg.session.keepalive = 60;
  cfg.network.disable_auto_reconnect = true;  // a reconnect needs a freshly signed URL
  cfg.network.timeout_ms = 20000;
  cfg.buffer.size = 2048;
  cfg.buffer.out_size = 512;
  cfg.task.stack_size = 8192;
  client = esp_mqtt_client_init(&cfg);
  if (!client) {
    esp_transport_destroy(tr);
    return false;
  }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
  esp_err_t e = esp_mqtt_client_start(client);
  if (e != ESP_OK) Serial.printf("[hydros] MQTT start: %s\n", esp_err_to_name(e));
  return e == ESP_OK;
}

// Dosing amounts. The status only says a head is running; the cloud keeps a
// dosing log per output (GET /logs?thingName=&name=, newest first, "time" in
// ms and the amount in "valueString", e.g. "... 2.5 ml", or "valueDec"; same
// call as pyhydros' get_dosing_logs). Read every 30 min for the picked dosing
// heads, while this task holds the TLS token and before the session opens.
float doseMl(JsonObjectConst r) {
  const char *vs = r["valueString"] | "";
  for (const char *p = vs; *p; p++) {
    if ((p[0] == 'm' || p[0] == 'M') && (p[1] == 'l' || p[1] == 'L')) {
      const char *q = p;
      while (q > vs && (q[-1] == ' ')) q--;
      const char *e = q;
      while (q > vs && (isdigit((unsigned char)q[-1]) || q[-1] == '.')) q--;
      if (q < e) return strtof(q, nullptr);
    }
  }
  for (const char *k : {"valueDec", "value"}) {
    JsonVariantConst v = r[k];
    if (v.is<float>() || v.is<int>()) return v.as<float>();
    if (v.is<const char *>()) {
      float f = strtof(v.as<const char *>(), nullptr);
      if (f > 0) return f;
    }
  }
  return 0;
}

void readDoseLogs() {
  uint32_t nowS = (uint32_t)time(nullptr);
  if (!idTok.length() || nowS + 60 > idTokExp) return;  // next real login brings a token
  if (doseLogsAtS && nowS - doseLogsAtS < 30 * 60 && doseLogsAtS <= nowS) return;
  doseLogsAtS = nowS;
  const Settings &s = settings::get();
  for (auto &o : s.outlets) {
    if (!o.key.startsWith("hydros:")) continue;
    if (!outlets::isDoser(o.key.c_str())) continue;  // only dosing heads have a dosing log
    int sl = o.key.indexOf('/');
    if (sl < 0) continue;
    String thing = o.key.substring(7, sl), name = o.key.substring(sl + 1);
    char q[48];
    snprintf(q, sizeof q, "&count=60&start=%llu", (unsigned long long)(nowS - 24UL * 3600UL) * 1000ULL);
    String url = String(kApi) + "/logs?thingName=" + sigv4::uriEncode(thing.c_str()).c_str() +
                 "&name=" + sigv4::uriEncode(name.c_str()).c_str() + q;
    JsonDocument filter;
    filter[0]["time"] = true;
    filter[0]["valueString"] = true;
    filter[0]["valueDec"] = true;
    filter[0]["value"] = true;
    JsonDocument d;
    https::Result r = https::requestJson("GET", url, {{"Authorization", idTok}, {"Content-Type", "application/json"}}, "",
                                         d, &filter);
    if (r.status != 200 || r.error.length()) {
      Serial.printf("[hydros] dosing log for %s: HTTP %d %s\n", name.c_str(), r.status, r.error.c_str());
      continue;
    }
    JsonArrayConst arr = d.as<JsonArrayConst>();
    int total = arr.size(), m = 0;
    if (total > 60) total = 60;
    uint32_t at[60];
    float ml[60];
    // Newest first in the reply: fill from the back so the arrays run oldest first.
    for (JsonObjectConst rec : arr) {
      if (m >= total) break;
      double t = rec["time"].is<double>() ? rec["time"].as<double>() : strtod(rec["time"] | "0", nullptr);
      if (t < 1.7e12) continue;
      at[m] = (uint32_t)(t / 1000.0);
      ml[m] = doseMl(rec);
      m++;
    }
    for (int i = 0; i < m / 2; i++) {
      std::swap(at[i], at[m - 1 - i]);
      std::swap(ml[i], ml[m - 1 - i]);
    }
    // Keep oldest-first order even if the reply wasn't sorted.
    for (int i = 1; i < m; i++)
      for (int j = i; j > 0 && at[j - 1] > at[j]; j--) {
        std::swap(at[j - 1], at[j]);
        std::swap(ml[j - 1], ml[j]);
      }
    bool set = m && outlets::setDoses(o.key.c_str(), at, ml, m);
    Serial.printf("[hydros] dosing log for %s: %d dose(s) in 24 h%s%s\n", name.c_str(), m,
                  m ? ", last " : "", m ? (String(ml[m - 1], 1) + " mL").c_str() : "");
    if (!set && m) Serial.printf("[hydros] (not shown: %s isn't on the Outlets page)\n", name.c_str());
  }
}

void loginJob(String user, String pass, uint8_t g) {
  String err;
  if (!tls::acquire("Hydros login")) {
    err = "waiting for the network (" + String(tls::holder()) + ")";
  } else {
    Serial.printf("[mem] before Hydros login: 8-bit free %u, largest %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    err = connectSteps(user, pass, false, nullptr);
    if (!err.length()) readDoseLogs();
    if (err.length()) tls::release();
    else tls::detach("Hydros session");  // kept until stopClient()
  }
  loginErr = err;
  loginGen = g;
  loginDone = true;  // last: the loop reads loginErr after seeing this
}

// The loop's side of a finished login.
void loginFinished(uint32_t now) {
  loginDone = false;
  String err = loginErr;
  if (loginGen != gen) {  // settings changed while it ran: throw it away
    if (!err.length()) tls::release();
    nextTry = 0;
    return;
  }
  // Safety net: TLS needs two ~17 kB blocks plus working space. If the heap is
  // too fragmented for that three times running, restart (the e-paper keeps
  // the last picture, so it's ~10 s with no visible change).
  static uint8_t tlsMemFails = 0;
  if (err.indexOf("ESP_ERR_HTTP_CONNECT") >= 0 && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 45000) {
    if (++tlsMemFails >= 3) {
      Serial.printf("[hydros] heap too fragmented for TLS (largest 8-bit block %u); restarting\n",
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      delay(200);
      ESP.restart();
    }
  } else if (!err.length()) {
    tlsMemFails = 0;
  }
  if (err.length()) {
    setState(err);
    nextTry = now + backoff;
    backoff = min<uint32_t>(backoff * 2, 10UL * 60UL * 1000UL);
    return;
  }
  holdsTls = true;
  sawConnect = false;
  Serial.printf("[hydros] signed in; %u controller(s), first %s; opening wss://%s (request %u bytes)\n",
                (unsigned)things.size(), things.empty() ? "-" : things[0].name.c_str(), wsHost.c_str(),
                (unsigned)wsPath.length());
  sessionStart = millis();
  if (!startClient()) {
    setState("MQTT start failed");
    stopClient();
    nextTry = now + backoff;
  }
}

}  // namespace

void loop() {
  const Settings &s = settings::get();
  if (loginDone) {
    // Start the session and come back next pass. (Build 17 carried on with a
    // `now` read before sessionStart was set: now - sessionStart wrapped to
    // ~49 days, so every new session looked expired and dropped, was torn
    // down right after subscribing, and the backoff doubled until Hydros
    // stopped trying.)
    loginFinished(millis());
    return;
  }
  uint32_t now = millis();
  if (!s.hydrosOn || s.mode != "direct") {
    if (client) stopClient();
    setState(s.hydrosOn ? "on (used in direct mode)" : "off");
    return;
  }
  if (client) {
    // Someone needs the network's one TLS connection (update check, a Test
    // button): step aside, log in again once they're done.
    if (tls::wanted()) {
      stopClient();
      setState("paused: the panel needs the network for a moment");
      nextTry = now + 1000;
      return;
    }
    // Signed differences: lastMsg is written by the MQTT task and can be a
    // little newer than `now`; unsigned it would wrap to "silent for 49 days".
    int32_t sinceStart = rules::since(now, sessionStart), sinceMsg = rules::since(now, lastMsg);
    // Saved credentials can have less than the usual hour left: renew 2 min
    // before they run out.
    bool expired = sinceStart > (int32_t)kRebuildMs || (credExp && time(nullptr) + 120 > credExp);
    bool silent = connected && sinceMsg > (int32_t)kSilentMs;
    bool dropped = !connected && sinceStart > 30000;
    if (expired || silent || dropped) {
      if (dropped && fromCache && !sawConnect) {
        Serial.println("[hydros] saved AWS credentials refused; signing in again");
        forgetCreds();
        fromCache = false;
        dropped = false;  // not a network problem: sign in again now, no backoff
        expired = true;
      }
      stopClient();
      setState(expired ? "renewing login" : silent ? "no data for 10 min, reconnecting" : "connection dropped, retrying");
      nextTry = now + (dropped ? backoff : 1000);
      if (dropped) backoff = min<uint32_t>(backoff * 2, 10UL * 60UL * 1000UL);
    } else {
      if (connected) {
        backoff = 15000;
        setState("ok · " + String(things.size()) + " controller" + (things.size() == 1 ? "" : "s"));
      } else {
        setState("connecting to AWS IoT");
      }
    }
    return;
  }
  if (worker::pending("hydros")) return;  // login on its way
  if ((int32_t)(now - nextTry) < 0) return;
  if (!s.hydrosUser.length() || !s.hydrosPass.length()) {
    setState("e-mail and password needed");
    nextTry = now + 30000;
    return;
  }
  if (!WiFi.isConnected()) return;
  if (time(nullptr) < 1700000000) {
    // SigV4 needs the clock; just booted, NTP answers within seconds.
    setState("waiting for the clock (NTP)");
    return;
  }
  setState("logging in");
  String user = s.hydrosUser, pass = s.hydrosPass;
  uint8_t g = gen;
  if (!worker::post("hydros", [user, pass, g] { loginJob(user, pass, g); })) nextTry = now + 1000;
}

void reserve() {
  if (!inflater.ready())
    Serial.printf("[hydros] couldn't reserve inflate memory (largest block %u)\n", (unsigned)ESP.getMaxAllocHeap());
}

void restart() {
  onlyFirst = false;
  stopClient();
  gen++;
  nextTry = 0;
  backoff = 15000;
}

String test(const String &user, const String &pass) {
  const Settings &s = settings::get();
  // Same login as the running session and it's streaming: that is the test.
  // A second TLS session next to the live one doesn't fit in RAM.
  if (user == s.hydrosUser && pass == s.hydrosPass && ok())
    return "ok · signed in and streaming (" + String(things.size()) + " controller" +
           (things.size() == 1 ? "" : "s") + ")";
  // Runs on the worker. Taking the token makes a live session step aside.
  if (!tls::acquire("Hydros test")) return "busy: " + String(tls::holder()) + " is using the network";
  String summary;
  String err = connectSteps(user, pass, true, &summary);
  tls::release();
  return err.length() ? err : "ok · " + summary;
}

String status() {
  if (connected && lastMsg) return state + " · last data " + String((uint32_t)max<int32_t>(0, (int32_t)(millis() - lastMsg)) / 1000) + " s ago";
  return state;
}
bool ok() { return connected && lastMsg && (int32_t)(millis() - lastMsg) < (int32_t)kSilentMs; }
uint32_t lastMsgMs() { return lastMsg; }

}  // namespace hydros
