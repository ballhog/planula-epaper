#include "SigV4.h"

#include <string.h>

#include <algorithm>
#include <vector>

namespace sigv4 {

// ── SHA-256 (FIPS 180-4) ────────────────────────────────────────────────
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

void sha256(const uint8_t *data, size_t n, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) block(h, data + i);
  uint8_t last[128] = {0};
  size_t rem = n - i;
  memcpy(last, data + i, rem);
  last[rem] = 0x80;
  size_t total = rem + 1 + 8 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) last[total - 1 - k] = (uint8_t)(bits >> (8 * k));
  block(h, last);
  if (total == 128) block(h, last + 64);
  for (int k = 0; k < 8; k++) {
    out[k * 4] = h[k] >> 24; out[k * 4 + 1] = h[k] >> 16; out[k * 4 + 2] = h[k] >> 8; out[k * 4 + 3] = h[k];
  }
}

void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t n, uint8_t out[32]) {
  uint8_t k[64] = {0};
  if (keyLen > 64) sha256(key, keyLen, k);
  else memcpy(k, key, keyLen);
  std::vector<uint8_t> inner(64 + n);
  for (int i = 0; i < 64; i++) inner[i] = k[i] ^ 0x36;
  memcpy(inner.data() + 64, msg, n);
  uint8_t ih[32];
  sha256(inner.data(), inner.size(), ih);
  uint8_t outer[96];
  for (int i = 0; i < 64; i++) outer[i] = k[i] ^ 0x5c;
  memcpy(outer + 64, ih, 32);
  sha256(outer, 96, out);
}

std::string hex(const uint8_t *d, size_t n) {
  static const char *x = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
  return s;
}

std::string uriEncode(const std::string &s, bool encodeSlash) {
  static const char *x = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (c == '/' && !encodeSlash)) o += (char)c;
    else { o += '%'; o += x[c >> 4]; o += x[c & 15]; }
  }
  return o;
}

static void hmacStr(const uint8_t *key, size_t kl, const std::string &m, uint8_t out[32]) {
  hmacSha256(key, kl, (const uint8_t *)m.data(), m.size(), out);
}

std::string presignQuery(const Presign &p) {
  std::string date = p.amzDate.substr(0, 8);
  std::string scope = date + "/" + p.region + "/" + p.service + "/aws4_request";
  std::vector<std::pair<std::string, std::string>> q = {
      {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
      {"X-Amz-Credential", p.accessKey + "/" + scope},
      {"X-Amz-Date", p.amzDate},
      {"X-Amz-SignedHeaders", "host"},
  };
  if (p.expires > 0) q.push_back({"X-Amz-Expires", std::to_string(p.expires)});
  if (!p.signedToken.empty()) q.push_back({"X-Amz-Security-Token", p.signedToken});
  std::sort(q.begin(), q.end());
  std::string cq;
  for (auto &kv : q) {
    if (!cq.empty()) cq += '&';
    cq += uriEncode(kv.first) + "=" + uriEncode(kv.second);
  }
  std::string ph = p.payloadHash;
  if (ph.empty()) {
    uint8_t e[32];
    sha256((const uint8_t *)"", 0, e);
    ph = hex(e, 32);
  }
  std::string creq = p.method + "\n" + uriEncode(p.path, false) + "\n" + cq + "\nhost:" + p.host + "\n\nhost\n" + ph;
  uint8_t ch[32];
  sha256((const uint8_t *)creq.data(), creq.size(), ch);
  std::string sts = "AWS4-HMAC-SHA256\n" + p.amzDate + "\n" + scope + "\n" + hex(ch, 32);

  std::string k0 = "AWS4" + p.secretKey;
  uint8_t kd[32], kr[32], ks[32], kq[32], sig[32];
  hmacStr((const uint8_t *)k0.data(), k0.size(), date, kd);
  hmacStr(kd, 32, p.region, kr);
  hmacStr(kr, 32, p.service, ks);
  hmacStr(ks, 32, "aws4_request", kq);
  hmacStr(kq, 32, sts, sig);
  return cq + "&X-Amz-Signature=" + hex(sig, 32);
}

std::string iotWebsocketPath(const std::string &host, const std::string &region, const std::string &accessKey,
                             const std::string &secretKey, const std::string &sessionToken,
                             const std::string &amzDate) {
  Presign p;
  p.host = host;
  p.path = "/mqtt";
  p.service = "iotdevicegateway";
  p.region = region;
  p.accessKey = accessKey;
  p.secretKey = secretKey;
  p.amzDate = amzDate;
  std::string q = presignQuery(p);
  if (!sessionToken.empty()) q += "&X-Amz-Security-Token=" + uriEncode(sessionToken);
  return "/mqtt?" + q;
}

}  // namespace sigv4
