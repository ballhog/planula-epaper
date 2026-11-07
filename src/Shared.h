// Shared state and functions of the panel's main loop, split over
// main.cpp (setup, loop, Coeno state), Screen.cpp (chrome, pages, drawing),
// Alarms.cpp, Keys.cpp (buttons) and Updates.cpp (check / install / upload).
// Everything here lives in namespace ui; the files that use it say
// `using namespace ui;`. The variables are defined in main.cpp.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>
#include <memory>
#include <new>
#include <vector>
#include "Apex.h"
#include "App.h"
#include "CoenoLink.h"
#include "DirectPages.h"
#include "Display.h"
#include "Https.h"
#include "Hydros.h"
#include "Net.h"
#include "Readings.h"
#include "Render.h"
#include "Settings.h"
#include "Web.h"
#include "Worker.h"
#include "Board.h"
#include "Power.h"
#include "ota.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace ui {

constexpr int PIN_BUTTON = 0;  // BOOT (Waveshare board)


extern int curPage, nPages;
extern uint32_t lastRotate, lastDraw, lastState, infoUntil, lastReminder;
extern volatile bool redrawAsked;  // also set from the worker
extern bool fullAsked, identifyAsked;
extern bool healthy;

// Alarms across all pages.
struct Alarm {
  String id, text;
};
extern std::vector<Alarm> alarms;
extern std::vector<String> knownAlarmIds;
extern bool acked;
extern bool flashAsked;

// Updates.
// Checks and installs run on the worker task (Worker.h); these are shared.
extern volatile long availBuild;
extern SharedText updateText;
extern volatile const char *updatingFrom;  // set while a download runs: draw() shows it
extern volatile int updatePct;  // GitHub download progress (Coeno: ota_download_pct())
extern volatile bool checking;  // a check is running on the worker
extern volatile bool updateFailed;  // an install just failed: open the System screen
extern volatile uint32_t checkedAtMs;  // last finished check
extern String availSource, availUrl, availSha;  // where the available build is (worker writes, then sets availBuild)
extern size_t availSize;
extern volatile bool autoInstallReady;
extern uint32_t lastCheck;
extern volatile bool retrySoon;  // last check couldn't connect
extern bool checkAsked, installAsked, rebootAsked, needsReboot;
extern uint32_t rebootAt;
extern String lastScreen;

// Screen.cpp
String clockText();
bool coenoMode();
bool coenoLinked();
int countPages();
bool fetchPage(int i, panel::Page &p);
void fillChrome(panel::Chrome &c);
display::Scene message(const String &title, const String &body, const String &big, const panel::Chrome &c);
display::Scene drawInfo(panel::Chrome &c);
display::Scene drawSystem(panel::Chrome &c);
void draw();

// Alarms.cpp
void collectAlarms();

// Keys.cpp
#if defined(BOARD_RETERMINAL)
void keysBegin();
void keys();
#else
void button();
#endif

// Updates.cpp
void netUpload();
void showUpdating(const char *from);
void checkUpdates();
void installUpdate();
void checkJob();
void installJob();
void updates();

// main.cpp
String faultText();
void onCoenoCommand(const String &leaf, const String &payload);

}  // namespace ui
