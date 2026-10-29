#include "Worker.h"

#include <freertos/queue.h>
#include <freertos/task.h>

namespace worker {

namespace {

struct Job {
  char tag[16];
  std::function<void()> fn;
};

constexpr int kQueue = 8;
QueueHandle_t q = nullptr;
SemaphoreHandle_t mu = nullptr;  // guards tags[] and cur
char tags[kQueue + 1][16];       // queued + running
char cur[16] = "";
TaskHandle_t task = nullptr;
constexpr uint32_t kStack = 12 * 1024;  // TLS handshakes run here

void removeTag(const char *t) {
  for (auto &s : tags)
    if (!strcmp(s, t)) {
      s[0] = 0;
      return;
    }
}

void run(void *) {
  for (;;) {
    Job *j = nullptr;
    if (xQueueReceive(q, &j, portMAX_DELAY) != pdTRUE || !j) continue;
    xSemaphoreTake(mu, portMAX_DELAY);
    strlcpy(cur, j->tag, sizeof cur);
    xSemaphoreGive(mu);
    j->fn();
    xSemaphoreTake(mu, portMAX_DELAY);
    cur[0] = 0;
    removeTag(j->tag);
    xSemaphoreGive(mu);
    delete j;
  }
}

}  // namespace

void begin() {
  if (task) return;
  mu = xSemaphoreCreateMutex();
  q = xQueueCreate(kQueue, sizeof(Job *));
  memset(tags, 0, sizeof tags);
  // Same core and priority as loop(): they take turns, and blocking socket
  // calls hand the CPU back while they wait.
  xTaskCreatePinnedToCore(run, "net", kStack, nullptr, 1, &task, 1);
}

bool post(const char *tag, std::function<void()> fn) {
  if (!task) begin();
  xSemaphoreTake(mu, portMAX_DELAY);
  bool dup = false;
  char *slot = nullptr;
  for (auto &s : tags) {
    if (!strcmp(s, tag)) dup = true;
    if (!s[0] && !slot) slot = s;
  }
  if (dup || !slot) {
    xSemaphoreGive(mu);
    return false;
  }
  Job *j = new (std::nothrow) Job();
  if (!j) {
    xSemaphoreGive(mu);
    return false;
  }
  strlcpy(j->tag, tag, sizeof j->tag);
  j->fn = std::move(fn);
  if (xQueueSend(q, &j, 0) != pdTRUE) {
    xSemaphoreGive(mu);
    delete j;
    return false;
  }
  strlcpy(slot, tag, 16);
  xSemaphoreGive(mu);
  return true;
}

bool pending(const char *tag) {
  if (!task) return false;
  xSemaphoreTake(mu, portMAX_DELAY);
  bool f = false;
  for (auto &s : tags) f |= !strcmp(s, tag);
  xSemaphoreGive(mu);
  return f;
}

const char *running() { return cur; }

uint32_t stackFree() { return task ? uxTaskGetStackHighWaterMark(task) : 0; }

}  // namespace worker

extern TaskHandle_t loopTaskHandle;  // Arduino core

namespace tls {

namespace {
SemaphoreHandle_t sem = nullptr;
TaskHandle_t owner = nullptr;
int depth = 0;
const char *who = "";
volatile int waiters = 0;

void init() {
  if (sem) return;
  sem = xSemaphoreCreateBinary();
  xSemaphoreGive(sem);
}
}  // namespace

bool acquire(const char *name, uint32_t waitMs) {
  init();
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  if (depth > 0 && owner == me) {
    depth++;
    return true;
  }
  if (me == loopTaskHandle) {
    // loop() must not wait for the token: the Hydros session gives it back
    // from loop(), so that would only ever time out. Network work belongs on
    // the worker (worker::post).
    Serial.printf("[tls] %s called from loop(); move it to the worker\n", name);
    waitMs = 0;
  }
  waiters++;
  bool got = xSemaphoreTake(sem, pdMS_TO_TICKS(waitMs)) == pdTRUE;
  waiters--;
  if (!got) {
    Serial.printf("[tls] %s gave up waiting for %s\n", name, who);
    return false;
  }
  owner = me;
  depth = 1;
  who = name;
  return true;
}

void release() {
  init();
  if (depth > 1) {
    depth--;
    return;
  }
  if (depth == 0) return;
  owner = nullptr;
  depth = 0;
  who = "";
  xSemaphoreGive(sem);
}

void detach(const char *name) {
  owner = nullptr;
  depth = 1;
  who = name;
}

bool wanted() { return waiters > 0; }
const char *holder() { return who; }

}  // namespace tls
