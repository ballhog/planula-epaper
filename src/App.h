// Shared state between main.cpp and the web page.
#pragma once
#include <Arduino.h>

namespace app {

int page();
int pages();
void gotoPage(int n);
void nextPage();
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

}  // namespace app
