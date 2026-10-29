// Direct mode: CoralVue Hydros through its cloud, the same path Coeno's
// hydros_service (pyhydros) takes:
//   1. Cognito USER_PASSWORD_AUTH login (Hydros app e-mail + password) → ID token
//   2. GET https://cv.hydros.link/user → user id + controllers ("things")
//   3. GET /thing/<id> → Cognito identity + AWS IoT endpoint
//   4. Cognito GetCredentialsForIdentity → temporary AWS keys (~1 h)
//   5. MQTT over a SigV4-signed WebSocket to AWS IoT; subscribe
//      <user>/<thing>/rsp/#, ask for status on <user>/<thing>/req/LISTEN/statusc
// Payloads are JSON, sometimes zlib-compressed behind a short header.
// Inputs go into readings as "hydros:<thing>/<input name>".
// The login (steps 1-4) runs on the worker task; loop() starts the session.
// The session is rebuilt every 50 min (before the AWS keys expire) and after
// 10 min with no message, like Coeno's watchdog.
#pragma once
#include <Arduino.h>

namespace hydros {

void loop();
void restart();     // drop the session; loop() logs in again
void reserve();     // grab the inflate buffers early, before the heap fragments
// Login + list controllers. Blocking: call it from the worker task. A live
// session steps aside while it runs (one TLS connection at a time).
String test(const String &user, const String &pass);
String status();
bool ok();
uint32_t lastMsgMs();

}  // namespace hydros
