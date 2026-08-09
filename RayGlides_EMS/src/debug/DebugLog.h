#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>

enum LogLevel { LOG_INFO, LOG_WARN, LOG_ERROR };

// Initializes the debug UART channel (reuses the main Serial/UART0 link).
// Call once from setup(), after Serial.begin().
void initDebugLog();

// Runtime on/off switch - lets the system silence debug output (e.g. in
// a production build) without recompiling, while still allowing an
// upstream command to re-enable it for field diagnostics.
void setDebugEnabled(bool enabled);
bool isDebugEnabled();

// Logs one line as: [timestamp ms][LEVEL][MODULE] message
// Filters out INFO-level messages when the minimum level is raised, so
// a noisy subsystem can be quieted without losing WARN/ERROR visibility.
void logMessage(LogLevel level, const char* module, const char* message);

// Convenience wrappers
void logInfo(const char* module, const char* message);
void logWarn(const char* module, const char* message);
void logError(const char* module, const char* message);

// Raises or lowers the minimum level that actually gets printed.
void setMinLogLevel(LogLevel level);

#endif
