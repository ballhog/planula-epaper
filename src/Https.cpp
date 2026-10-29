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

}  // namespace https
