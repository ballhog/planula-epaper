// Background network work, and the one-TLS-connection rule.
//
// worker: one FreeRTOS task that runs network jobs one at a time (Apex poll,
// Hydros login, update check and install, the Test buttons). The main loop
// only queues them, so drawing, the web page and the button never wait on a
// slow network call. Builds up to 16 ran everything in loop(): a 15 s Apex
// timeout or a 10 s Hydros login froze the settings page ("write(): fail").
//
// tls: the ESP32 has RAM for one TLS connection (~40 kB with mbedtls in and
// out buffers), so opening one takes a token. Every https:: call takes it
// itself, so nothing can forget. The Hydros AWS session keeps it for as long
// as it runs and gives it up when someone else asks (tls::wanted()).
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>

namespace worker {

void begin();
// Queue a job. tag names it: a job whose tag is already queued or running is
// not queued again (returns false).
bool post(const char *tag, std::function<void()> fn);
bool pending(const char *tag);
const char *running();      // tag of the job running now, "" when idle
uint32_t stackFree();       // lowest free stack seen, bytes

}  // namespace worker

namespace tls {

// Take the token, waiting up to waitMs (Hydros is asked to step aside while
// someone waits). Nested calls from the task holding it just count.
bool acquire(const char *who, uint32_t waitMs = 30000);
void release();
// Keep the token after the job ends, for a long-lived connection that no task
// owns (the Hydros session). Any task may release() it later.
void detach(const char *who);
bool wanted();              // someone is waiting for the token
const char *holder();       // "" when free

}  // namespace tls

// A String shared between the worker and the main loop.
class SharedText {
 public:
  SharedText(const char *init = "") : v_(init) {}
  void set(const String &s) {
    lock();
    v_ = s;
    unlock();
  }
  String get() {
    lock();
    String c = v_;
    unlock();
    return c;
  }

 private:
  void lock() {
    if (!m_) m_ = xSemaphoreCreateMutex();
    xSemaphoreTake(m_, portMAX_DELAY);
  }
  void unlock() { xSemaphoreGive(m_); }
  SemaphoreHandle_t m_ = nullptr;
  String v_;
};
