#include "Hydros.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <mqtt_client.h>
#include <time.h>

#include <vector>

#include "Https.h"
#include "Readings.h"
#include "Settings.h"
#include "SigV4.h"
#include "miniz.h"

namespace hydros {

namespace {

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
uint32_t sessionStart = 0, nextTry = 0, backoff = 15000;
String state = "off", userId;
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
  const char *keys[] = {"iotEndpoint", "iot_endpoint", "mqttEndpoint", "mqtt_endpoint", "awsIotEndpoint",
                        "aws_iot_endpoint", "iotEndpointAddress"};
  for (const char *k : keys)
    if (v[k].is<const char *>()) return v[k].as<const char *>();
  for (JsonPairConst kv : v.as<JsonObjectConst>())
    if (kv.value().is<JsonObjectConst>())
      for (const char *k : keys)
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

https::Result api(const String &path, const String &idToken) {
  return https::request("GET", String(kApi) + path, {{"Authorization", idToken}, {"Content-Type", "application/json"}},
                        "", 65536);
}

// Steps 1-4. On success fills things, userId, wsHost, wsPath.
String connectSteps(const String &user, const String &pass, bool forTest, String *summary) {
  if (!WiFi.isConnected()) return "no Wi-Fi";
  if (time(nullptr) < 1700000000) return "waiting for the clock (NTP)";
  Login L = login(user, pass);
  if (L.error.length()) return L.error;

  https::Result u = api("/user", L.idToken);
  if (u.status != 200) return "Hydros /user failed (HTTP " + String(u.status) + ")";
  JsonDocument ud;
  if (deserializeJson(ud, u.body)) return "Hydros /user: bad reply";
  u.body = String();
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

  https::Result t = api(String("/thing/") + sigv4::uriEncode(top[0].id.c_str()).c_str(), L.idToken);
  if (t.status != 200) return "Hydros /thing failed (HTTP " + String(t.status) + ")";
  JsonDocument td;
  deserializeJson(td, t.body);
  t.body = String();
  String identity = td["cognito_identity"] | "";
  String parent = td["parent"] | "";
  if (!endpoint.length()) endpoint = findEndpoint(td.as<JsonVariantConst>());
  if (!identity.length() && parent.length()) {
    https::Result p = api("/thing/" + parent, L.idToken);
    JsonDocument pd;
    deserializeJson(pd, p.body);
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

  JsonDocument cb;
  cb["IdentityId"] = identity;
  cb["Logins"]["cognito-idp." + L.region + ".amazonaws.com/" + L.pool] = L.idToken;
  String body;
  serializeJson(cb, body);
  https::Result c = https::request("POST", "https://cognito-identity." + region + ".amazonaws.com/",
                                   {{"Content-Type", "application/x-amz-json-1.1"},
                                    {"X-Amz-Target", "AWSCognitoIdentityService.GetCredentialsForIdentity"}},
                                   body, 16384);
  if (c.status != 200) return "AWS credentials refused (HTTP " + String(c.status) + ")";
  JsonDocument cd;
  deserializeJson(cd, c.body);
  String ak = cd["Credentials"]["AccessKeyId"] | "", sk = cd["Credentials"]["SecretKey"] | "",
         tok = cd["Credentials"]["SessionToken"] | "";
  if (!ak.length() || !sk.length()) return "no AWS credentials in reply";

  wsHost = endpoint;
  wsPath = sigv4::iotWebsocketPath(endpoint.c_str(), region.c_str(), ak.c_str(), sk.c_str(), tok.c_str(),
                                   amzDate().c_str())
               .c_str();
  return "";
}

float numberOf(JsonVariantConst v) {
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  return NAN;
}

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
  uint8_t *json = p;
  size_t jlen = n;
  uint8_t *out = nullptr;
  if (n > 2 && p[0] == 0x78) {
    size_t cap = 96 * 1024;
    out = (uint8_t *)malloc(cap);
    tinfl_decompressor *d = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    if (!out || !d) { free(out); free(d); return; }
    tinfl_init(d);
    size_t inLen = n, outLen = cap;
    tinfl_status st = tinfl_decompress(d, p, &inLen, out, out, &outLen,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    if (st != TINFL_STATUS_DONE) { free(out); return; }
    json = out;
    jlen = outLen;
  }
  JsonDocument filter;
  filter["Input"]["*"]["probeValue"] = true;
  filter["Input"]["*"]["senseValue"] = true;
  filter["Input"]["*"]["i10Value"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char *)json, jlen, DeserializationOption::Filter(filter));
  free(out);
  if (err) return;
  lastMsg = millis();
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
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
      lastMsg = millis();
      for (auto &t : things) {
        String rspT = userId + "/" + t.id + "/rsp/#";
        String reqT = userId + "/" + t.id + "/req/LISTEN/statusc";
        esp_mqtt_client_subscribe(e->client, rspT.c_str(), 1);
        esp_mqtt_client_publish(e->client, reqT.c_str(), "", 0, 1, 0);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset == 0) {
        rxTopic = String(e->topic, e->topic_len);
        rxLen = 0;
        size_t need = e->total_data_len;
        if (need > 64 * 1024) { rxCap = 0; break; }  // too big to hold: skip
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
}

bool startClient() {
  clientIdStr = "planula-" + String((uint32_t)ESP.getEfuseMac(), HEX) + "-" + String(esp_random() % 10000);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = wsHost.c_str();
  cfg.broker.address.port = 443;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_WSS;
  cfg.broker.address.path = wsPath.c_str();
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.credentials.client_id = clientIdStr.c_str();
  cfg.session.keepalive = 60;
  cfg.network.disable_auto_reconnect = true;  // a reconnect needs a freshly signed URL
  cfg.network.timeout_ms = 20000;
  cfg.buffer.size = 4096;
  cfg.buffer.out_size = 1024;
  cfg.task.stack_size = 12288;
  client = esp_mqtt_client_init(&cfg);
  if (!client) return false;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
  return esp_mqtt_client_start(client) == ESP_OK;
}

}  // namespace

void loop() {
  const Settings &s = settings::get();
  if (!s.hydrosOn || s.mode != "direct") {
    if (client) stopClient();
    state = s.hydrosOn ? "on (used in direct mode)" : "off";
    return;
  }
  uint32_t now = millis();
  if (client) {
    bool expired = now - sessionStart > kRebuildMs;
    bool silent = connected && now - lastMsg > kSilentMs;
    bool dropped = !connected && now - sessionStart > 30000;
    if (expired || silent || dropped) {
      stopClient();
      state = expired ? "renewing login" : silent ? "no data for 10 min, reconnecting" : "connection dropped, retrying";
      nextTry = now + (dropped ? backoff : 1000);
      if (dropped) backoff = min<uint32_t>(backoff * 2, 10UL * 60UL * 1000UL);
    } else {
      if (connected) {
        backoff = 15000;
        state = "ok · " + String(things.size()) + " controller" + (things.size() == 1 ? "" : "s");
      } else {
        state = "connecting to AWS IoT";
      }
    }
    return;
  }
  if ((int32_t)(now - nextTry) < 0) return;
  if (!s.hydrosUser.length() || !s.hydrosPass.length()) {
    state = "e-mail and password needed";
    nextTry = now + 30000;
    return;
  }
  state = "logging in";
  String err = connectSteps(s.hydrosUser, s.hydrosPass, false, nullptr);
  if (err.length()) {
    state = err;
    nextTry = now + backoff;
    backoff = min<uint32_t>(backoff * 2, 10UL * 60UL * 1000UL);
    return;
  }
  sessionStart = millis();
  if (!startClient()) {
    state = "MQTT start failed";
    stopClient();
    nextTry = now + backoff;
  }
}

void restart() {
  stopClient();
  nextTry = 0;
  backoff = 15000;
}

String test(const String &user, const String &pass) {
  String summary;
  String err = connectSteps(user, pass, true, &summary);
  return err.length() ? err : "ok · " + summary;
}

String status() {
  if (connected && lastMsg) return state + " · last data " + String((millis() - lastMsg) / 1000) + " s ago";
  return state;
}
bool ok() { return connected && lastMsg && millis() - lastMsg < kSilentMs; }
uint32_t lastMsgMs() { return lastMsg; }

}  // namespace hydros
