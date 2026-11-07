// Shared state between main.cpp and the web page.
#pragma once
#include <Arduino.h>

#include "Model.h"

namespace app {

int page();
int pages();
void gotoPage(int n);
void nextPage();
void prevPage();
void toggleSchedule();
void togglePage(int n);  // a button for page n (0-based), pressed again: back  // to the Outlets / Schedule page and back
// Tiles: number + trend -> graphs -> numbers only -> ... (saved).
void cycleView();
void redraw(bool full);  // redraw soon (full = flashing refresh)
void ackAlarm();
String alarmsJson();     // [{"id","text"}]
bool alarmAcked();
void identify();         // flash the screen so you can find it

long availableBuild();   // > 0 when Coeno has a newer build
String updateCheckText();
void checkUpdateSoon();
void installUpdateSoon();
void rebootSoon();
bool rebootNeeded();
void setRebootNeeded();

// For power (battery modes):
bool currentPage(panel::Page &p);  // the page on screen, freshly built
void drawNow();                    // collect alarms and draw at once (full refresh)
uint32_t alarmSig();               // hash of the alarm ids now showing
// After a deep-sleep wake: re-collect alarms and keep "acknowledged" if the
// same alarms were acknowledged before the sleep.
void syncAlarms(bool wasAcked, uint32_t ackedSig);
bool updating();                   // a firmware download/install is running
bool hasUnackedAlarm();

}  // namespace app
