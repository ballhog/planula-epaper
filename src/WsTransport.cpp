#include "WsTransport.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <esp_transport_ssl.h>
#include <mbedtls/base64.h>

#include <string>

namespace wst {

namespace {

struct Ctx {
  esp_transport_handle_t ssl = nullptr;
  std::string path, subprotocol;
  std::string pre;        // bytes read past the HTTP headers during the handshake
  size_t prePos = 0;
  uint64_t remaining = 0; // payload bytes left in the current data frame
};

Ctx *ctxOf(esp_transport_handle_t t) { return (Ctx *)esp_transport_get_context_data(t); }

// Raw read: leftover handshake bytes first, then TLS.
int rawRead(Ctx *c, char *buf, int len, int timeoutMs) {
  if (c->prePos < c->pre.size()) {
    int n = std::min<int>(len, c->pre.size() - c->prePos);
    memcpy(buf, c->pre.data() + c->prePos, n);
    c->prePos += n;
    return n;
  }
  return esp_transport_read(c->ssl, buf, len, timeoutMs);
}

// Read exactly n bytes; firstTimeout for the first byte (0 = "nothing yet"),
// then a generous timeout for the rest of a frame already started.
int readExact(Ctx *c, uint8_t *buf, int n, int firstTimeout) {
  int got = 0;
  while (got < n) {
    int r = rawRead(c, (char *)buf + got, n - got, got == 0 ? firstTimeout : 5000);
    if (r < 0) return -1;
    if (r == 0) {
      if (got == 0) return 0;  // timeout before anything arrived
      return -1;               // stalled mid-frame
    }
    got += r;
  }
  return got;
}

int writeAll(Ctx *c, const uint8_t *buf, int len, int timeoutMs) {
  int sent = 0;
  while (sent < len) {
    int w = esp_transport_write(c->ssl, (const char *)buf + sent, len - sent, timeoutMs);
    if (w <= 0) return -1;
    sent += w;
  }
  return sent;
}

int sendFrame(Ctx *c, uint8_t opcode, const uint8_t *data, size_t len, int timeoutMs) {
  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | opcode;  // FIN
  if (len < 126) hdr[h++] = 0x80 | len;
  else if (len < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = len >> 8;
    hdr[h++] = len & 0xFF;
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint64_t)len >> (8 * i);
  }
  uint8_t mask[4];
  esp_fill_random(mask, 4);
  memcpy(hdr + h, mask, 4);
  h += 4;
  std::string frame((const char *)hdr, h);
  frame.resize(h + len);
  for (size_t i = 0; i < len; i++) frame[h + i] = data[i] ^ mask[i & 3];
  return writeAll(c, (const uint8_t *)frame.data(), frame.size(), timeoutMs) < 0 ? -1 : (int)len;
}

int wsConnect(esp_transport_handle_t t, const char *host, int port, int timeoutMs) {
  Ctx *c = ctxOf(t);
  c->pre.clear();
  c->prePos = 0;
  c->remaining = 0;
  if (esp_transport_connect(c->ssl, host, port, timeoutMs) < 0) {
    Serial.printf("[ws] TLS connect to %s:%d failed\n", host, port);
    return -1;
  }
  uint8_t keyRaw[16], key[32];
  size_t klen = 0;
  esp_fill_random(keyRaw, sizeof keyRaw);
  mbedtls_base64_encode(key, sizeof key, &klen, keyRaw, sizeof keyRaw);
  std::string req = "GET " + c->path + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + std::string((const char *)key, klen) + "\r\nSec-WebSocket-Version: 13\r\n";
  if (!c->subprotocol.empty()) req += "Sec-WebSocket-Protocol: " + c->subprotocol + "\r\n";
  req += "User-Agent: planula-epaper\r\n\r\n";
  if (writeAll(c, (const uint8_t *)req.data(), req.size(), timeoutMs) < 0) {
    Serial.println("[ws] sending the upgrade request failed");
    return -1;
  }
  // Read the response headers (stop at the blank line; keep anything after it).
  std::string resp;
  char buf[256];
  uint32_t start = millis();
  size_t end;
  while ((end = resp.find("\r\n\r\n")) == std::string::npos) {
    if (resp.size() > 4096 || millis() - start > (uint32_t)timeoutMs) {
      Serial.println("[ws] no upgrade response");
      return -1;
    }
    int r = esp_transport_read(c->ssl, buf, sizeof buf, 1000);
    if (r < 0) {
      Serial.println("[ws] connection closed during upgrade");
      return -1;
    }
    resp.append(buf, r);
  }
  c->pre = resp.substr(end + 4);
  std::string status = resp.substr(0, resp.find("\r\n"));
  if (status.find(" 101") == std::string::npos) {
    Serial.printf("[ws] upgrade refused: %s\n", status.c_str());
    return -1;
  }
  return 0;
}

int wsRead(esp_transport_handle_t t, char *buf, int len, int timeoutMs) {
  Ctx *c = ctxOf(t);
  while (c->remaining == 0) {
    uint8_t h[2];
    int r = readExact(c, h, 2, timeoutMs);
    if (r <= 0) return r;  // 0 = timeout, nothing arrived
    uint8_t op = h[0] & 0x0F;
    bool masked = h[1] & 0x80;
    uint64_t n = h[1] & 0x7F;
    if (n == 126) {
      uint8_t e[2];
      if (readExact(c, e, 2, 5000) <= 0) return -1;
      n = (e[0] << 8) | e[1];
    } else if (n == 127) {
      uint8_t e[8];
      if (readExact(c, e, 8, 5000) <= 0) return -1;
      n = 0;
      for (int i = 0; i < 8; i++) n = (n << 8) | e[i];
    }
    uint8_t mask[4] = {0};
    if (masked && readExact(c, mask, 4, 5000) <= 0) return -1;
    if (op == 0x8) {  // close
      Serial.println("[ws] server closed the connection");
      return -1;
    }
    if (op == 0x9 || op == 0xA) {  // ping / pong: control payload ≤ 125
      uint8_t p[125];
      if (n > sizeof p || (n && readExact(c, p, n, 5000) <= 0)) return -1;
      if (op == 0x9) sendFrame(c, 0xA, p, n, 5000);
      continue;
    }
    c->remaining = n;  // data (binary/text/continuation): payload is the MQTT byte stream
    if (n == 0) continue;
  }
  int want = (int)std::min<uint64_t>(len, c->remaining);
  int r = rawRead(c, buf, want, timeoutMs);
  if (r > 0) c->remaining -= r;
  return r;
}

int wsWrite(esp_transport_handle_t t, const char *buf, int len, int timeoutMs) {
  return sendFrame(ctxOf(t), 0x2, (const uint8_t *)buf, len, timeoutMs);
}

int wsPollRead(esp_transport_handle_t t, int timeoutMs) {
  Ctx *c = ctxOf(t);
  if (c->prePos < c->pre.size()) return 1;
  return esp_transport_poll_read(c->ssl, timeoutMs);
}

int wsPollWrite(esp_transport_handle_t t, int timeoutMs) { return esp_transport_poll_write(ctxOf(t)->ssl, timeoutMs); }

int wsClose(esp_transport_handle_t t) {
  Ctx *c = ctxOf(t);
  c->remaining = 0;
  c->pre.clear();
  c->prePos = 0;
  return esp_transport_close(c->ssl);
}

int wsDestroy(esp_transport_handle_t t) {
  Ctx *c = ctxOf(t);
  if (c) {
    if (c->ssl) esp_transport_destroy(c->ssl);
    delete c;
  }
  return 0;
}

}  // namespace

esp_transport_handle_t create(const char *path, const char *subprotocol) {
  esp_transport_handle_t t = esp_transport_init();
  if (!t) return nullptr;
  Ctx *c = new Ctx();
  c->path = path ? path : "/";
  c->subprotocol = subprotocol ? subprotocol : "";
  c->ssl = esp_transport_ssl_init();
  esp_transport_ssl_crt_bundle_attach(c->ssl, esp_crt_bundle_attach);
  esp_transport_set_context_data(t, c);
  esp_transport_set_func(t, wsConnect, wsRead, wsWrite, wsClose, wsPollRead, wsPollWrite, wsDestroy);
  esp_transport_set_default_port(t, 443);
  return t;
}

}  // namespace wst
