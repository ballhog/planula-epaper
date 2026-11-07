// reTerminal E1001 extras: SHT4x room sensor and battery. On the Waveshare
// board these are no-ops.
#pragma once
#include <Arduino.h>

namespace board {

void begin();
void loop();             // reads the sensor and battery once a minute
bool hasBattery();       // a battery is connected (reads above 3.0 V)
int batteryPct();        // 0-100, -1 unknown
float batteryVolts();    // 0 unknown
String status();         // "room 24.1 °C 48 % · battery 87 % (4.02 V)"
// Buzzer (GPIO45): three short beeps for an alarm; silence() stops it.
// Off when the Display setting "Beep on alarms" is off.
void alarmBeep();
void silence();
bool beeping();

}  // namespace board
