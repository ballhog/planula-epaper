// AWS Signature V4 query-string presigning, self-contained (own SHA-256/HMAC)
// so it runs on the panel and in the host tests (tools/host) alike.
// Used to open the Hydros (AWS IoT) MQTT-over-WebSocket connection.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace sigv4 {

void sha256(const uint8_t *data, size_t n, uint8_t out[32]);
void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t n, uint8_t out[32]);
std::string hex(const uint8_t *d, size_t n);
std::string uriEncode(const std::string &s, bool encodeSlash = true);

struct Presign {
  std::string method = "GET";
  std::string host, path = "/", service, region;
  std::string accessKey, secretKey;
  std::string amzDate;        // "20130524T000000Z" (UTC)
  int expires = 0;            // X-Amz-Expires; 0 = leave out
  std::string payloadHash;    // "UNSIGNED-PAYLOAD" (S3) or "" = SHA-256 of empty body
  std::string signedToken;    // session token inside the signature (most services)
};

// Canonical query string including X-Amz-Signature.
std::string presignQuery(const Presign &p);

// AWS IoT over WebSocket: signed URL path + query, with the session token
// appended *after* signing (IoT's rule). Returns "/mqtt?X-Amz-...".
std::string iotWebsocketPath(const std::string &host, const std::string &region, const std::string &accessKey,
                             const std::string &secretKey, const std::string &sessionToken,
                             const std::string &amzDate);

}  // namespace sigv4
