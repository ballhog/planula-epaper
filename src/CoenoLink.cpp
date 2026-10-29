#include "CoenoLink.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>

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

panel::Page pages[panel::kMaxPages];
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
panel::Page staging;  // parse target (only the MQTT task touches it)

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
  }
  String s;
  serializeJson(d, s);
  return s;
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
    if (panel::parsePage(data, len, staging)[0]) return;
    Lock l;
    pages[n] = staging;
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
  }
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      isConnected = true;
      String h = hello(true);
      esp_mqtt_client_publish(e->client, (prefix + "hello").c_str(), h.c_str(), h.length(), 1, 1);
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
}

}  // namespace

void loop() {
  const Settings &s = settings::get();
  // The link stays up in both modes when a host is set: Coeno also gets the
  // panel's state and can send commands / updates in direct mode.
  bool want = WiFi.isConnected() && s.coenoHost.length() > 0;
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
  if (!pageValid[i]) return false;
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
