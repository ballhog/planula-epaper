#include "CoenoLink.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>

#include <new>
#include <vector>

#include "PageJson.h"
#include "Settings.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace coeno {

namespace {

esp_mqtt_client_handle_t client = nullptr;
volatile bool isConnected = false;
String prefix, uri, lwt, clientId, activeKey;
SemaphoreHandle_t mu = nullptr;

// Allocated on first start (Coeno mode only): ~23 kB the panel needs in direct
// mode for TLS.
panel::Page *pages = nullptr;
uint32_t pageAt[panel::kMaxPages] = {0};
bool pageValid[panel::kMaxPages] = {false};
int cfgPages = -1;
uint16_t cfgRotate = 0;
uint32_t lastPage = 0;

std::vector<std::pair<String, String>> pending;  // commands for loop()
void (*cmdCb)(const String &, const String &) = nullptr;

char *rx = nullptr;
size_t rxCap = 0;
String rxTopic;
panel::Page *staging = nullptr;  // parse target (only the MQTT task touches it)

struct Lock {
  Lock() {
    if (!mu) mu = xSemaphoreCreateMutex();
    xSemaphoreTake(mu, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(mu); }
};

String hello(bool online) {
  JsonDocument d;
  d["online"] = online;
  if (online) {
    d["type"] = "planula-epaper";
    d["node"] = settings::get().name;
    d["protocol_version"] = 1;
    d["board"] = NODE_BOARD;
    d["build"] = FW_BUILD;
    d["version"] = FW_VERSION;
    d["ip"] = WiFi.localIP().toString();
    d["mac"] = WiFi.macAddress();
    d["mode"] = settings::get().mode;
    d["width"] = 800;
    d["height"] = 480;
    d["describe_rev"] = "b" + String(FW_BUILD);   // the description only changes with the firmware
  }
  String s;
  serializeJson(d, s);
  return s;
}

// Coeno's device standard (protocol/devices.py in Coeno): who this panel is and
// what Coeno may do with it. Same shape as Coeno's adapter for older builds.
String describe() {
  JsonDocument doc;
  doc["standard"] = 1;
  JsonObject d = doc["device"].to<JsonObject>();
  d["serial"] = settings::get().name;
  d["model"] = "planula-epaper";
  d["source"] = "planula";
  d["name"] = settings::get().name;
  d["hw_rev"] = NODE_BOARD;
  d["mac"] = WiFi.macAddress();
  JsonObject fw = d["firmware"].to<JsonObject>();
  fw["build"] = FW_BUILD;
  fw["version"] = FW_VERSION;
  fw["repo"] = "ballhog/planula-epaper";
  d["page"] = "/";
  d["schedule"] = nullptr;
  JsonObject n = doc["nodes"]["screen"].to<JsonObject>();
  n["name"] = "Screen";
  JsonObject pr = n["properties"].to<JsonObject>();
  JsonObject p = pr["page"].to<JsonObject>();
  p["role"] = "setting"; p["name"] = "Page shown"; p["datatype"] = "integer";
  p["min"] = 0; p["max"] = panel::kMaxPages - 1; p["settable"] = true; p["key"] = "page";
  p = pr["pages"].to<JsonObject>();
  p["role"] = "status"; p["name"] = "Pages"; p["datatype"] = "integer"; p["key"] = "pages";
  p = pr["rssi"].to<JsonObject>();
  p["role"] = "status"; p["name"] = "WiFi signal"; p["datatype"] = "integer"; p["unit"] = "dBm"; p["key"] = "rssi";
  JsonObject ac = n["actions"].to<JsonObject>();
  for (const char *a : {"next", "refresh", "identify", "ack", "check_update"}) ac[a].to<JsonObject>();
  String out;
  serializeJson(doc, out);
  return out;
}

void standardEvent(const char *kind, const String &path, const char *why) {
  if (!client) return;
  JsonDocument d;
  d["kind"] = kind;
  d["path"] = path;
  d["ok"] = why == nullptr;
  if (why) d["why"] = why;
  String s;
  serializeJson(d, s);
  esp_mqtt_client_publish(client, (prefix + "event").c_str(), s.c_str(), s.length(), 1, 0);
}

void handle(const String &topic, const char *data, size_t len) {
  String leaf = topic.substring(prefix.length());  // "page/0", "config", "cmd/next"
  if (leaf.startsWith("page/")) {
    int n = leaf.substring(5).toInt();
    if (n < 0 || n >= panel::kMaxPages) return;
    if (len == 0) {  // retained page cleared
      Lock l;
      pageValid[n] = false;
      return;
    }
    if (!pages || !staging || panel::parsePage(data, len, *staging)[0]) return;
    Lock l;
    pages[n] = *staging;
    pageAt[n] = millis();
    pageValid[n] = true;
    lastPage = millis();
  } else if (leaf == "config") {
    JsonDocument d;
    if (deserializeJson(d, data, len)) return;
    Lock l;
    cfgPages = d["pages"] | -1;
    cfgRotate = d["rotate_s"] | 0;
  } else if (leaf.startsWith("cmd/")) {
    Lock l;
    if (pending.size() < 8) pending.push_back({leaf.substring(4), String(data, len)});
  } else if (leaf.startsWith("set/") || leaf.startsWith("do/")) {
    // Device standard: set/screen/page {"value": n}; do/screen/<next|refresh|identify|ack|check_update>.
    bool isSet = leaf.startsWith("set/");
    String path = leaf.substring(isSet ? 4 : 3);
    String cmd, body = "{}";
    if (isSet && path == "screen/page") {
      JsonDocument d;
      if (deserializeJson(d, data, len) || !d["value"].is<int>()) { standardEvent("set", path, "send {\"value\": n}"); return; }
      int n = d["value"];
      if (n < 0 || n >= panel::kMaxPages) { standardEvent("set", path, "no such page"); return; }
      cmd = "page";
      body = "{\"n\":" + String(n) + "}";
    } else if (!isSet && path.startsWith("screen/")) {
      cmd = path.substring(7);
      if (cmd == "check_update") cmd = "check";
      if (cmd != "next" && cmd != "refresh" && cmd != "identify" && cmd != "ack" && cmd != "check") {
        standardEvent("do", path, "no such action");
        return;
      }
    } else {
      standardEvent(isSet ? "set" : "do", path, isSet ? "not settable over MQTT" : "no such action");
      return;
    }
    {
      Lock l;
      if (pending.size() < 8) pending.push_back({cmd, body});
    }
    standardEvent(isSet ? "set" : "do", path, nullptr);
  }
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      isConnected = true;
      Serial.printf("[coeno] connected to %s\n", uri.c_str());
      String dsc = describe();   // before hello: hello says the description is there
      esp_mqtt_client_publish(e->client, (prefix + "describe").c_str(), dsc.c_str(), dsc.length(), 1, 1);
      String h = hello(true);
      esp_mqtt_client_publish(e->client, (prefix + "hello").c_str(), h.c_str(), h.length(), 1, 1);
      esp_mqtt_client_subscribe(e->client, (prefix + "set/#").c_str(), 1);
      esp_mqtt_client_subscribe(e->client, (prefix + "do/#").c_str(), 1);
      esp_mqtt_client_subscribe(e->client, (prefix + "page/+").c_str(), 1);
      esp_mqtt_client_subscribe(e->client, (prefix + "config").c_str(), 1);
      esp_mqtt_client_subscribe(e->client, (prefix + "cmd/#").c_str(), 1);
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      isConnected = false;
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset == 0) {
        rxTopic = String(e->topic, e->topic_len);
        if ((size_t)e->total_data_len + 1 > rxCap) {
          free(rx);
          rx = (char *)malloc(e->total_data_len + 1);
          rxCap = rx ? e->total_data_len + 1 : 0;
        }
      }
      if (!rx || (size_t)(e->current_data_offset + e->data_len) >= rxCap) break;
      memcpy(rx + e->current_data_offset, e->data, e->data_len);
      if (e->current_data_offset + e->data_len == e->total_data_len) {
        rx[e->total_data_len] = 0;
        handle(rxTopic, rx, e->total_data_len);
      }
      break;
    }
    default:
      break;
  }
}

void stop() {
  if (client) {
    if (isConnected) {
      String h = hello(false);
      esp_mqtt_client_publish(client, (prefix + "hello").c_str(), h.c_str(), h.length(), 1, 1);
      delay(100);
    }
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  isConnected = false;
  activeKey = "";
}

void start() {
  const Settings &s = settings::get();
  if (!pages) {
    pages = new (std::nothrow) panel::Page[panel::kMaxPages];
    staging = new (std::nothrow) panel::Page();
    if (!pages || !staging) {
      Serial.println("[coeno] no memory for pages");
      delete[] pages;
      delete staging;
      pages = nullptr;
      staging = nullptr;
      return;
    }
  }
  prefix = "panel_node/" + s.name + "/";
  uri = "mqtt://" + s.coenoHost + ":" + String(s.coenoPort);
  lwt = hello(false);
  clientId = s.name;
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri.c_str();
  cfg.credentials.client_id = clientId.c_str();
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = nullptr;
  static String lwtTopic;
  lwtTopic = prefix + "hello";
  cfg.session.last_will.topic = lwtTopic.c_str();
  cfg.session.last_will.msg = lwt.c_str();
  cfg.session.last_will.msg_len = lwt.length();
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.buffer.size = 4096;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.task.stack_size = 8192;
  client = esp_mqtt_client_init(&cfg);
  if (!client) return;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
  esp_mqtt_client_start(client);
  activeKey = s.name + "@" + uri;
  Serial.printf("[coeno] connecting to %s (clear the Coeno address to stop)\n", uri.c_str());
}

}  // namespace

void loop() {
  const Settings &s = settings::get();
  // The link stays up in both modes when a host is set: Coeno also gets the
  // panel's state and can send commands / updates in direct mode.
  // Pages over MQTT are only used in Coeno mode. In direct mode the client
  // (task + buffers) would only cost the RAM TLS needs; updates from Coeno are
  // plain HTTP and keep working without it.
  bool want = WiFi.isConnected() && s.coenoHost.length() > 0 && s.mode == "coeno";
  String key = s.name + "@mqtt://" + s.coenoHost + ":" + String(s.coenoPort);
  if (!want || (client && key != activeKey)) stop();
  if (want && !client) start();

  std::vector<std::pair<String, String>> todo;
  {
    Lock l;
    todo.swap(pending);
  }
  for (auto &c : todo)
    if (cmdCb) cmdCb(c.first, c.second);
}

void restart() { stop(); }
bool connected() { return isConnected; }

String status() {
  const Settings &s = settings::get();
  if (!s.coenoHost.length()) return "no Coeno host set";
  if (s.mode != "coeno") return "not used in direct mode (updates still checked at " + s.coenoHost + ")";
  if (!isConnected) return "connecting to " + s.coenoHost;
  if (!lastPage) return "connected · no page yet";
  return "connected · last page " + String((millis() - lastPage) / 1000) + " s ago";
}

int pageCount() {
  Lock l;
  if (cfgPages >= 0) return min(cfgPages, panel::kMaxPages);
  int n = 0;
  for (int i = 0; i < panel::kMaxPages; i++)
    if (pageValid[i]) n = i + 1;
  return n;
}

bool getPage(int i, panel::Page &out, uint32_t &receivedMs) {
  if (i < 0 || i >= panel::kMaxPages) return false;
  Lock l;
  if (!pages || !pageValid[i]) return false;
  out = pages[i];
  receivedMs = pageAt[i];
  return true;
}

uint32_t lastPageMs() { return lastPage; }
uint16_t rotateSeconds() { return cfgRotate; }

void publishState(const String &json) {
  if (client && isConnected) esp_mqtt_client_publish(client, (prefix + "state").c_str(), json.c_str(), json.length(), 0, 1);
}
void publishAck(const String &json) {
  if (client && isConnected) esp_mqtt_client_publish(client, (prefix + "ack").c_str(), json.c_str(), json.length(), 1, 0);
}
void onCommand(void (*cb)(const String &, const String &)) { cmdCb = cb; }

}  // namespace coeno
