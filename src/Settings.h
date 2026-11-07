// Everything the user sets on the panel's own page, kept in NVS as one JSON blob.
//
// Two data modes:
//   coeno  — Coeno builds the pages (panel_node/<name>/page/<n>) and decides status.
//   direct — the panel reads the Apex (local REST) and/or Hydros (cloud) itself
//            and builds its pages from the gauges picked here.
// Firmware updates come from Coeno in both modes when a Coeno host is set.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

struct WifiNet {
  String ssid, pass;
};

struct PageCfg {
  String title = "Tank";
  String layout = "auto";
};

struct GaugeCfg {
  String key;      // reading key: "apex:<input name>" or "hydros:<thing>/<input name>"
  String label;    // shown name ("" = the reading's own name)
  String unit;     // "" = the reading's own unit
  uint8_t dec = 1;
  uint8_t page = 0;
  float warnLo = NAN, warnHi = NAN;    // normal band
  float alarmLo = NAN, alarmHi = NAN;  // alarm beyond these
};

// An output picked for the Outlets page (direct mode).
struct OutletCfg {
  String key;    // "apex:Return"
  String label;  // "" = its own name
};

struct Settings {
  String name = "planula-epaper-1";  // hostname + MQTT id
  String mode = "direct";            // coeno | direct (new panels: no Coeno address yet)
  std::vector<WifiNet> wifi;         // tried strongest-first

  String coenoHost;  // "" = standalone (no Coeno) until one is set; saved hosts are kept
  uint16_t coenoPort = 1883;
  String coenoUser, coenoPass;  // broker login (Coeno issues it when the panel is adopted)
  bool coenoUpdates = true;  // check Coeno for firmware
  bool githubUpdates = true; // no Coeno answering: check GitHub releases instead
  bool autoUpdate = false;   // install a newer build by itself (else: Install button / Coeno)

  bool apexOn = false;
  String apexHost, apexUser = "admin", apexPass;
  uint16_t apexPoll_s = 60;
  String apexTempUnit = "auto";  // auto | F | C (what the Apex reports)

  bool hydrosOn = false;
  String hydrosUser, hydrosPass;

  String tz = "EST5EDT,M3.2.0,M11.1.0";  // POSIX TZ (America/Detroit)
  bool clock24 = false;
  // Battery power (reTerminal; see Power.h): live | saver | daily.
  String power = "live";
  uint16_t saverMin = 10;
  String dailyAt = "07:00";
  bool hourlyCheck = true;
  bool buzzer = true;
  // reTerminal short presses: prev | next | view | refresh | info | none.
  String keyLeft = "prev", keyMid = "view", keyRight = "next";
  String keyAck = "middle";  // which press acknowledges an alarm first: middle | any
  std::vector<OutletCfg> outlets;  // Outlets page lanes, in order (max 8)  // reTerminal: beep on a new alarm and every 5 min until acknowledged
  String view = "both";  // both | numbers | graphs (tile view; a button cycles it on the reTerminal)
  bool tempF = false;        // direct mode: show temperatures in °F
  uint16_t rotate_s = 0;     // 0 = pages only change with the button
  uint16_t stale_s = 600;    // a value older than this shows as stale
  uint8_t fullEvery = 60;    // full (flashing) refresh after this many partial ones (~hourly)
  String panelDriver = "t7"; // t7 (GxEPD2_750_T7) | gdey (GDEY075T7): panel batches differ

  std::vector<PageCfg> pages;
  std::vector<GaugeCfg> gauges;

  String adminPass;  // settings page password (user "admin"); defaults to apPass, never blank
  String apPass;     // setup Wi-Fi password, made once per board; survives factoryReset()
                     // so a printed label stays valid
};

namespace settings {

Settings &get();
void load();
bool save();
// Erase everything but the setup Wi-Fi password (apPass); the settings
// password goes back to it.
void factoryReset();

// JSON for the web page. Passwords become {"set": true} unless withSecrets.
void toJson(JsonObject o, bool withSecrets);
// Apply a (partial) JSON update. Password fields left out or "" keep their
// value; {"clear": true} clears them (the settings password goes back to the
// setup Wi-Fi password instead). Returns "" or an error.
String applyJson(JsonObjectConst o);

}  // namespace settings
