#ifndef FAULT_DETECTION_H
#define FAULT_DETECTION_H

#include "fault/FaultManager.h"

const char* faultName(FaultCode f);
bool isCriticalFault(FaultCode f);

#endif
