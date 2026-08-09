#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include <Arduino.h>
#include "FaultDetection.h"

inline void initFaultLog() {}
inline void logFaultEvent(FaultCode code, Severity sev) {
  triggerFault(code, sev);
}
inline int getFaultLogCount() { return 0; }
inline void clearFaultLog() { clearAllFaults(); }
inline void printFaultLog() { printFaultHistory(); }

#endif
