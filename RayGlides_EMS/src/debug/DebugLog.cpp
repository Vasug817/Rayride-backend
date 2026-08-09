#include "DebugLog.h"

static bool debugEnabled = true;
static LogLevel minLevel = LOG_INFO;

void initDebugLog() {
  debugEnabled = true;
  minLevel = LOG_INFO;
  Serial.println("[DEBUG] UART debug logging initialized");
}

void setDebugEnabled(bool enabled) {
  debugEnabled = enabled;
}

bool isDebugEnabled() {
  return debugEnabled;
}

void setMinLogLevel(LogLevel level) {
  minLevel = level;
}

static const char* levelTag(LogLevel level) {
  switch (level) {
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
  }
  return "?";
}

void logMessage(LogLevel level, const char* module, const char* message) {
  if (!debugEnabled) return;
  if (level < minLevel) return;   // Filtered out by the current minimum level

  Serial.print("[");
  Serial.print(millis());
  Serial.print("ms][");
  Serial.print(levelTag(level));
  Serial.print("][");
  Serial.print(module);
  Serial.print("] ");
  Serial.println(message);
}

void logInfo(const char* module, const char* message)  { logMessage(LOG_INFO, module, message); }
void logWarn(const char* module, const char* message)  { logMessage(LOG_WARN, module, message); }
void logError(const char* module, const char* message) { logMessage(LOG_ERROR, module, message); }
