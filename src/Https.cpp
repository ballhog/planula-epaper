#include "Https.h"

#include <esp_crt_bundle.h>
#include <Update.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>

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

// Open (send body), read headers, and follow up to 5 redirects. Returns the
// final status, or < 0 on a transport error (err filled in).
int openFollow(esp_http_client_handle_t c, const String &body, String &err) {
  for (int hop = 0; hop < 6; hop++) {
    esp_err_t e = esp_http_client_open(c, body.length());
    if (e != ESP_OK) {
      err = String("connect failed: ") + esp_err_to_name(e);
      return -2;
    }
    if (body.length()) esp_http_client_write(c, body.c_str(), body.length());
    esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
      esp_http_client_flush_response(c, nullptr);
      esp_http_client_set_redirection(c);
      esp_http_client_close(c);
      continue;
    }
    return st;
  }
  err = "too many redirects";
  return -4;
}

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
  cfg.buffer_size = 4096;  // long redirect URLs (GitHub)
  cfg.buffer_size_tx = 2048;
  cfg.method = !strcmp(method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { r.status = -1; r.error = "init failed"; return r; }
  for (auto &h : headers) esp_http_client_set_header(c, h.first.c_str(), h.second.c_str());
  r.status = openFollow(c, body, r.error);
  if (r.status < 0) {
    esp_http_client_cleanup(c);
    return r;
  }
  long long len = esp_http_client_get_content_length(c);
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

String downloadFirmware(const String &url, const String &shaHex, size_t size, void (*progress)(int)) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 4096;     // GitHub's CDN redirect URLs are long
  cfg.buffer_size_tx = 2048;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return "init failed";
  String err;
  int st = openFollow(c, "", err);
  if (st != 200) {
    esp_http_client_cleanup(c);
    return st < 0 ? err : "HTTP " + String(st);
  }
  int64_t len = esp_http_client_get_content_length(c);
  if (size && len > 0 && (size_t)len != size) {
    esp_http_client_cleanup(c);
    return "size mismatch";
  }
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    esp_http_client_cleanup(c);
    return String("no room: ") + Update.errorString();
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t buf[2048];
  size_t total = 0;
  int lastPct = -1;
  while (true) {
    int n = esp_http_client_read(c, (char *)buf, sizeof buf);
    if (n < 0) { err = "download interrupted"; break; }
    if (n == 0) break;
    mbedtls_sha256_update(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) { err = Update.errorString(); break; }
    total += n;
    if (progress && len > 0) {
      int pct = (int)(total * 100 / len);
      if (pct / 10 != lastPct / 10) progress(pct);
      lastPct = pct;
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!err.length() && size && total != size) err = "short download";
  if (!err.length() && shaHex.length()) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (!shaHex.equalsIgnoreCase(hex)) err = "checksum mismatch";
  }
  if (err.length()) {
    Update.abort();
    return err;
  }
  if (!Update.end(true)) return Update.errorString();
  return "";
}

}  // namespace https
