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
#include "WsTransport.h"
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
bool onlyFirst = false;  // AWS dropped us after asking every controller: ask the first only
uint32_t sessionStart = 0, nextTry = 0, backoff = 15000;
String state = "off", userId;
String lastLogged;

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

// Steps 1-4. On success fills things, userId, wsHost, wsPath.
String connectSteps(const String &user, const String &pass, bool forTest, String *summary) {
  if (!WiFi.isConnected()) return "no Wi-Fi";
  if (time(nullptr) < 1700000000) return "waiting for the clock (NTP)";
  Login L = login(user, pass);
  if (L.error.length()) return L.error;

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

// Streaming zlib → ArduinoJson reader. The decompressor (~11 kB) and its
// 32 kB window are allocated on first use and kept, so a long session doesn't
// fragment the heap message by message.
struct InflateReader {
  tinfl_decompressor *d = nullptr;
  uint8_t *dict = nullptr;
  const uint8_t *in = nullptr;
  size_t inLeft = 0;
  size_t outPos = 0, readPos = 0, readEnd = 0;  // window positions
  bool done = false, failed = false;
  int status = 0;

  bool ready() {
    // Window first: it needs the bigger contiguous block.
    if (!dict) dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
    if (!d) d = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    return d && dict;
  }
  void start(const uint8_t *data, size_t n) {
    tinfl_init(d);
    in = data;
    inLeft = n;
    outPos = readPos = readEnd = 0;
    done = failed = false;
    status = 0;
  }
  bool refill() {
    if (done) return false;
    size_t inSize = inLeft, outSize = TINFL_LZ_DICT_SIZE - outPos;
    tinfl_status st = tinfl_decompress(d, in, &inSize, dict, dict + outPos, &outSize,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | (inLeft > inSize ? TINFL_FLAG_HAS_MORE_INPUT : 0));
    in += inSize;
    inLeft -= inSize;
    readPos = outPos;
    readEnd = outPos + outSize;
    outPos = (outPos + outSize) & (TINFL_LZ_DICT_SIZE - 1);
    status = (int)st;
    if (st == TINFL_STATUS_DONE) done = true;
    else if (st < 0) { failed = true; done = true; }
    else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && inLeft == 0) { failed = true; done = true; }
    return readEnd > readPos;
  }
  int read() {
    while (readPos >= readEnd)
      if (!refill()) return -1;
    return dict[readPos++];
  }
  size_t readBytes(char *b, size_t n) {
    size_t k = 0;
    while (k < n) {
      int c = read();
      if (c < 0) break;
      b[k++] = (char)c;
    }
    return k;
  }
};
InflateReader inflater;

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
  JsonDocument doc;
  DeserializationError err;
  if (n > 2 && p[0] == 0x78) {
    // Inflate as a stream through a fixed 32 kB window (allocated once), so the
    // size of the JSON inside doesn't matter. See InflateReader.
    if (!inflater.ready()) {
      Serial.printf("[hydros] no memory to inflate (largest free block %u bytes)\n", (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    inflater.start(p, n);
    err = deserializeJson(doc, inflater, DeserializationOption::Filter(filter));
    if (inflater.failed) {
      Serial.printf("[hydros] %s: zlib failed (%d)\n", topic.c_str(), inflater.status);
      return;
    }
  } else {
    err = deserializeJson(doc, (const char *)p, n, DeserializationOption::Filter(filter));
  }
  if (err) {
    Serial.printf("[hydros] %s: %u bytes, JSON %s (free %u)\n", topic.c_str(), (unsigned)len, err.c_str(),
                  (unsigned)ESP.getFreeHeap());
    return;
  }
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
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
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

}  // namespace

void loop() {
  const Settings &s = settings::get();
  if (!s.hydrosOn || s.mode != "direct") {
    if (client) stopClient();
    setState(s.hydrosOn ? "on (used in direct mode)" : "off");
    return;
  }
  uint32_t now = millis();
  if (client) {
    bool expired = now - sessionStart > kRebuildMs;
    bool silent = connected && now - lastMsg > kSilentMs;
    bool dropped = !connected && now - sessionStart > 30000;
    if (expired || silent || dropped) {
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
  if ((int32_t)(now - nextTry) < 0) return;
  if (!s.hydrosUser.length() || !s.hydrosPass.length()) {
    setState("e-mail and password needed");
    nextTry = now + 30000;
    return;
  }
  setState("logging in");
  String err = connectSteps(s.hydrosUser, s.hydrosPass, false, nullptr);
  if (err.length()) {
    setState(err);
    nextTry = now + backoff;
    backoff = min<uint32_t>(backoff * 2, 10UL * 60UL * 1000UL);
    return;
  }
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

void reserve() {
  if (!inflater.ready())
    Serial.printf("[hydros] couldn't reserve inflate memory (largest block %u)\n", (unsigned)ESP.getMaxAllocHeap());
}

void restart() {
  onlyFirst = false;
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
