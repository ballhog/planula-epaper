// Wi-Fi for the panel.
//   - Joins the strongest saved network (up to 6 saved).
//   - No network saved, or none reachable for 60 s at boot: opens its own
//     setup network "Planula-XXXX" (WPA2, password shown on the screen) with a
//     captive portal at http://192.168.4.1. It keeps retrying the saved
//     networks in the background and closes the setup network once joined.
//   - mDNS <name>.local (+ _calyx._tcp, type=panel), NTP with the panel's TZ.
#pragma once
#include <Arduino.h>

namespace net {

enum class State { Connecting, Online, SetupAp };

void begin();
void loop();
State state();
bool online();
String ip();
String apName();
String ssid();
int rssi();
String statusText();

// Rescan + rejoin (after Wi-Fi settings change).
void reconnect();
// Open the setup network now (from the settings page or a long button press).
void openSetup(const char *reason);
String setupReason();

// Last scan for the settings page: JSON array [{ssid, rssi, secure}].
String scanJson();

bool timeValid();

}  // namespace net
