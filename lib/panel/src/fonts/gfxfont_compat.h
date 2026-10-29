#pragma once
// Lets the generated font headers compile both on the ESP32 (Adafruit GFX) and
// in the host renderer (tools/host), which uses the same Adafruit GFX source.
#include <Adafruit_GFX.h>
#ifndef PROGMEM
#define PROGMEM
#endif
