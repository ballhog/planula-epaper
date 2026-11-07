// The panel's log: everything printed with plog goes to the USB serial port as
// before AND into a small ring kept in RAM, so the settings page can show the
// last few kB (Settings > Log). On battery there is no USB cable to read.
//
// Use plog.printf / println / print instead of Serial for log lines. Anything
// secret (the setup network password printed at boot) stays on Serial only.
#pragma once
#include <Arduino.h>

class LogTee : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t n) override;
  void flush() override { Serial.flush(); }
};

extern LogTee plog;

namespace logbuf {
// The kept log, oldest first (at most `max` bytes of the newest lines).
String text(size_t max = 0);
size_t capacity();
}  // namespace logbuf
