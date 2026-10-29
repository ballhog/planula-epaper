#include "Https.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>

namespace https {

Result request(const char *method, const String &url, const Headers &headers, const String &body, size_t maxBody,
               int timeoutMs) {
  Result r;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = timeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  if (!strcmp(method, "POST")) cfg.method = HTTP_METHOD_POST;
  else if (!strcmp(method, "PUT")) cfg.method = HTTP_METHOD_PUT;
  else cfg.method = HTTP_METHOD_GET;

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    r.status = -1;
    r.error = "init failed";
    return r;
  }
  for (auto &h : headers) esp_http_client_set_header(c, h.first.c_str(), h.second.c_str());

  esp_err_t err = esp_http_client_open(c, body.length());
  if (err != ESP_OK) {
    r.status = -2;
    r.error = String("connect failed: ") + esp_err_to_name(err);
    esp_http_client_cleanup(c);
    return r;
  }
  if (body.length() && esp_http_client_write(c, body.c_str(), body.length()) < 0) {
    r.status = -3;
    r.error = "send failed";
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return r;
  }
  esp_http_client_fetch_headers(c);
  r.status = esp_http_client_get_status_code(c);
  char buf[512];
  while (true) {
    int n = esp_http_client_read(c, buf, sizeof buf);
    if (n <= 0) break;
    if (r.body.length() + n > maxBody) {
      r.error = "reply too large";
      break;
    }
    r.body.concat(buf, n);
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return r;
}

namespace {
// ArduinoJson custom reader over esp_http_client_read (handles chunked replies).
struct Reader {
  esp_http_client_handle_t c;
  char buf[512];
  int len = 0, pos = 0;
  size_t total = 0;
  int read() {
    if (pos >= len) {
      len = esp_http_client_read(c, buf, sizeof buf);
      pos = 0;
      if (len <= 0) { len = 0; return -1; }
      total += len;
    }
    return (uint8_t)buf[pos++];
  }
  size_t readBytes(char *b, size_t n) {
    size_t k = 0;
    while (k < n) {
      int ch = read();
      if (ch < 0) break;
      b[k++] = (char)ch;
    }
    return k;
  }
};
}  // namespace

Result requestJson(const char *method, const String &url, const Headers &headers, const String &body,
                   JsonDocument &out, const JsonDocument *filter, int timeoutMs) {
  Result r;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = timeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  cfg.method = !strcmp(method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { r.status = -1; r.error = "init failed"; return r; }
  for (auto &h : headers) esp_http_client_set_header(c, h.first.c_str(), h.second.c_str());
  esp_err_t err = esp_http_client_open(c, body.length());
  if (err != ESP_OK) {
    r.status = -2;
    r.error = String("connect failed: ") + esp_err_to_name(err);
    esp_http_client_cleanup(c);
    return r;
  }
  if (body.length()) esp_http_client_write(c, body.c_str(), body.length());
  long long len = esp_http_client_fetch_headers(c);
  r.status = esp_http_client_get_status_code(c);
  Reader rd{c};
  DeserializationError de = filter ? deserializeJson(out, rd, DeserializationOption::Filter(*filter))
                                   : deserializeJson(out, rd);
  if (de) {
    // Drain what's left so the size in the message is the real one.
    while (rd.read() >= 0) {}
    r.error = String(de.c_str()) + ", " + String((unsigned)(rd.total / 1024)) + " kB" +
              (len > 0 ? " of " + String((unsigned)(len / 1024)) + " kB" : String(""));
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return r;
}

}  // namespace https
