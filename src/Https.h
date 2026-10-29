// Small blocking HTTPS client (ESP-IDF esp_http_client + the built-in root
// certificate bundle). Used for the Hydros cloud login; the Apex is plain HTTP.
#pragma once
#include <Arduino.h>

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

}  // namespace https
