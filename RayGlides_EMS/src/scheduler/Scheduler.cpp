#include "Scheduler.h"

static EMSTask tasks[MAX_TASKS];
static int taskCount = 0;

void initScheduler() {
  taskCount = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    tasks[i].name = nullptr;
    tasks[i].intervalMs = 0;
    tasks[i].lastRunMs = 0;
    tasks[i].callback = nullptr;
    tasks[i].enabled = false;
  }
}

bool addTask(const char* name, uint32_t intervalMs, void (*callback)()) {
  if (taskCount >= MAX_TASKS) return false;
  tasks[taskCount].name = name;
  tasks[taskCount].intervalMs = intervalMs;
  tasks[taskCount].lastRunMs = millis();
  tasks[taskCount].callback = callback;
  tasks[taskCount].enabled = true;
  taskCount++;
  return true;
}

void runScheduler() {
  uint32_t now = millis();
  for (int i = 0; i < taskCount; i++) {
    if (tasks[i].enabled && tasks[i].callback != nullptr) {
      if (now - tasks[i].lastRunMs >= tasks[i].intervalMs) {
        tasks[i].callback();
        tasks[i].lastRunMs = now;
      }
    }
  }
}
