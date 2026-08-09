#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>

#define MAX_TASKS 10

struct EMSTask {
  const char* name;
  uint32_t intervalMs;
  uint32_t lastRunMs;
  void (*callback)();
  bool enabled;
};

void initScheduler();
bool addTask(const char* name, uint32_t intervalMs, void (*callback)());
void runScheduler();

#endif
