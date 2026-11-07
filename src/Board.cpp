#include "Board.h"

#if defined(BOARD_RETERMINAL)

#include <Wire.h>

#include "Readings.h"
#include "Settings.h"

namespace board {

namespace {

// Seeed's reTerminal E docs: SHT4x at 0x44 on SDA 19 / SCL 20; battery on
// GPIO1 through a 1:2 divider, measured only while GPIO21 is high.
constexpr int kSda = 19, kScl = 20, kBatAdc = 1, kBatEnable = 21, kBuzzer = 45;

// Beeps: on/off steps in ms, driven from loop() so nothing blocks.
const uint16_t kPattern[] = {140, 110, 140, 110, 140};
int beepStep = -1;
uint32_t beepAt = 0;
constexpr uint8_t kSht = 0x44;

bool shtOk = false;
float roomC = NAN, roomRh = NAN, volts = 0;
int pct = -1;
uint32_t lastRead = 0;

uint8_t crc8(const uint8_t *d, int n) {  // Sensirion: poly 0x31, init 0xFF
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = c & 0x80 ? (c << 1) ^ 0x31 : c << 1;
  }
  return c;
}

bool readSht(float &tC, float &rh) {
  Wire.beginTransmission(kSht);
  Wire.write(0xFD);  // measure, high precision (~9 ms)
  if (Wire.endTransmission() != 0) return false;
  delay(12);
  uint8_t b[6];
  if (Wire.requestFrom((int)kSht, 6) != 6) return false;
  for (auto &x : b) x = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  tC = -45.0f + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
  rh = constrain(-6.0f + 125.0f * ((b[3] << 8) | b[4]) / 65535.0f, 0.0f, 100.0f);
  return true;
}

// Resting Li-ion voltage to charge, roughly. Good enough for a footer.
int pctFor(float v) {
  static const float V[] = {3.30f, 3.50f, 3.62f, 3.70f, 3.76f, 3.82f, 3.90f, 3.98f, 4.06f, 4.15f};
  static const int P[] = {0, 5, 15, 30, 45, 55, 65, 75, 88, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 10; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

void readBattery() {
  digitalWrite(kBatEnable, HIGH);
  delay(5);
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(kBatAdc);
  digitalWrite(kBatEnable, LOW);
  volts = mv / 8 / 1000.0f * 2.0f;
  pct = volts > 3.0f ? pctFor(volts) : -1;
}

void readAll() {
  const Settings &s = settings::get();
  float t, h;
  if (shtOk && readSht(t, h)) {
    roomC = t;
    roomRh = h;
    // Inside the case, next to the ESP32: reads a little warm when awake.
    readings::update("panel:room-temp", "Panel", s.name, "Room temperature", "temp", "°C", t);
    readings::update("panel:room-rh", "Panel", s.name, "Room humidity", "rh", "%", h);
  }
  readBattery();
  if (pct >= 0) readings::update("panel:battery", "Panel", s.name, "Panel battery", "battery", "%", pct);
}

}  // namespace

void begin() {
  pinMode(kBatEnable, OUTPUT);
  digitalWrite(kBatEnable, LOW);
  analogReadResolution(12);
  analogSetPinAttenuation(kBatAdc, ADC_11db);
  Wire.begin(kSda, kScl);
  Wire.beginTransmission(kSht);
  shtOk = Wire.endTransmission() == 0;
  readAll();
  lastRead = millis();
  Serial.printf("[board] room sensor %s; battery %.2f V (%d %%)\n", shtOk ? "found" : "NOT found", volts, pct);
}

void loop() {
  if (beepStep >= 0 && millis() - beepAt >= kPattern[beepStep]) {
    beepAt = millis();
    if (++beepStep >= (int)(sizeof kPattern / sizeof kPattern[0])) {
      noTone(kBuzzer);
      beepStep = -1;
    } else if (beepStep % 2 == 0) {
      tone(kBuzzer, 2700);
    } else {
      noTone(kBuzzer);
    }
  }
  if (millis() - lastRead < 60000) return;
  lastRead = millis();
  readAll();
}

void alarmBeep() {
  if (!settings::get().buzzer) return;
  tone(kBuzzer, 2700);
  beepStep = 0;
  beepAt = millis();
}

void silence() {
  if (beepStep >= 0) noTone(kBuzzer);
  beepStep = -1;
}

bool beeping() { return beepStep >= 0; }

bool hasBattery() { return pct >= 0; }
int batteryPct() { return pct; }
float batteryVolts() { return volts; }

String status() {
  String o;
  if (!isnan(roomC)) o = "room " + String(roomC, 1) + " °C " + String(roomRh, 0) + " %";
  else o = shtOk ? "room sensor: no reading" : "room sensor not found";
  o += pct >= 0 ? " · battery " + String(pct) + " % (" + String(volts, 2) + " V)" : " · no battery";
  return o;
}

}  // namespace board

#else  // Waveshare board: nothing on board

namespace board {
void begin() {}
void loop() {}
bool hasBattery() { return false; }
int batteryPct() { return -1; }
float batteryVolts() { return 0; }
String status() { return ""; }
void alarmBeep() {}
void silence() {}
bool beeping() { return false; }
}  // namespace board

#endif
