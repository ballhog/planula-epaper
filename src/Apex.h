// Direct mode: Neptune Apex over its local REST API (same call Coeno makes):
//   GET http://<apex>/rest/status   Basic auth (Apex web login)
// Inputs (probes, Trident, power) go into readings as "apex:<input name>".
// Temperatures are stored in °C (Apex reports °F or °C; see apexTempUnit).
#pragma once
#include <Arduino.h>

namespace apex {

void loop();               // polls on its own schedule when enabled
void pollNow();            // next loop() polls immediately
// One poll with the given login, for the "Test" button. Returns a status line.
String test(const String &host, const String &user, const String &pass);
String status();           // "ok · 14 inputs · 20 s ago", "login refused", ...
bool ok();
uint32_t lastOkMs();

}  // namespace apex
