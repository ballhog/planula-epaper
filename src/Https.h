// Small blocking HTTPS client (ESP-IDF esp_http_client + the built-in root
// certificate bundle). Used for the Hydros cloud login; the Apex is plain HTTP.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <utility>
#include <vector>

namespace https {

struct Result {
  int status = 0;   // HTTP status, or < 0 on a transport error
  String body;
  String error;     // set when status < 0
};

using Headers = std::vector<std::pair<String, String>>;

Result request(const char *method, const String &url, const Headers &headers, const String &body,
               size_t maxBody = 32768, int timeoutMs = 15000);

// Same, but decodes the JSON reply while it downloads, keeping only what
// `filter` allows (ArduinoJson filter; nullptr = everything). For big replies
// (the Hydros /user profile carries every controller's setup). Result.body is
// left empty; Result.error says why decoding failed ("NoMemory, 83 kB", ...).
Result requestJson(const char *method, const String &url, const Headers &headers, const String &body,
                   JsonDocument &out, const JsonDocument *filter, int timeoutMs = 20000);

// Download url (following redirects) into the inactive app slot, checking the
// SHA-256 before switching. progress(pct) is optional. Returns "" or an error.
String downloadFirmware(const String &url, const String &sha256Hex, size_t size, void (*progress)(int pct));

}  // namespace https
