// Minimal Arduino shim so lib/panel + Adafruit GFX build on the host (tools/host).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
typedef bool boolean;
typedef uint8_t byte;
#define _swap_int16_t(a, b) { int16_t t = a; a = b; b = t; }
#include "Print.h"
class String {
 public:
  String(const char *s = "") : s_(s) {}
  const char *c_str() const { return s_; }
  unsigned length() const { return strlen(s_); }
 private:
  const char *s_;
};
