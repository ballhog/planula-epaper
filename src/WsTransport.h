// A small WebSocket-over-TLS transport for esp-mqtt, used for AWS IoT (Hydros).
//
// Why: the framework's own WebSocket transport builds the upgrade request in a
// fixed CONFIG_WS_BUFFER_SIZE (1024 B) buffer, compiled into the Arduino core's
// libraries. AWS IoT's SigV4-signed URL with a Cognito session token is ~1.9 kB,
// so the built-in one fails with "Error in request generation". This one sends
// the request from a heap string of any length, then does plain RFC 6455
// framing (binary frames, client masking, ping→pong, close).
#pragma once
#include <esp_transport.h>

namespace wst {

// path = "/mqtt?X-Amz-..." (sent as-is). Hand the handle to esp-mqtt as
// cfg.network.transport; esp-mqtt destroys it with the client.
esp_transport_handle_t create(const char *path, const char *subprotocol);

}  // namespace wst
