#include "Log.h"

#include <freertos/FreeRTOS.h>
#include <time.h>

LogTee plog;

namespace {

#if defined(BOARD_HAS_PSRAM) || defined(BOARD_RETERMINAL)
constexpr size_t kCap = 16 * 1024;
#else
constexpr size_t kCap = 4 * 1024;  // the Waveshare board has no spare RAM
#endif
char ring[kCap];
size_t head = 0, used = 0;
bool lineStart = true;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void put(char c) {
  ring[head] = c;
  head = (head + 1) % kCap;
  if (used < kCap) used++;
}

}  // namespace

size_t LogTee::write(const uint8_t *buf, size_t n) {
  size_t w = Serial.write(buf, n);
  // Each line starts with the time ("09:26:19 ") or, before the clock is set,
  // seconds since boot ("+12s ").
  char stamp[16];
  stamp[0] = 0;
  portENTER_CRITICAL(&mux);
  for (size_t i = 0; i < n; i++) {
    char c = (char)buf[i];
    if (c == '\r') continue;
    if (lineStart && c != '\n') {
      if (!stamp[0]) {
        portEXIT_CRITICAL(&mux);
        time_t now = time(nullptr);
        if (now > 1700000000) {
          struct tm tm;
          localtime_r(&now, &tm);
          strftime(stamp, sizeof stamp, "%H:%M:%S ", &tm);
        } else {
          snprintf(stamp, sizeof stamp, "+%lus ", (unsigned long)(millis() / 1000));
        }
        portENTER_CRITICAL(&mux);
      }
      for (const char *p = stamp; *p; p++) put(*p);
    }
    put(c);
    lineStart = c == '\n';
  }
  portEXIT_CRITICAL(&mux);
  return w;
}

namespace logbuf {

size_t capacity() { return kCap; }

String text(size_t max) {
  String out;
  portENTER_CRITICAL(&mux);
  size_t n = used, start = (head + kCap - used) % kCap;
  portEXIT_CRITICAL(&mux);
  if (max && n > max) {
    start = (start + (n - max)) % kCap;
    n = max;
  }
  out.reserve(n + 1);
  // Copy in chunks outside the lock; a line written meanwhile may tear, which is fine for a log view.
  for (size_t i = 0; i < n; i++) out += ring[(start + i) % kCap];
  // Start at a whole line.
  int nl = out.indexOf('\n');
  if (n == kCap && nl >= 0) out.remove(0, nl + 1);
  return out;
}

}  // namespace logbuf
