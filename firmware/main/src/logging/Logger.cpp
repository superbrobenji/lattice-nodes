#include "Logger.h"

namespace lattice {
namespace utils {

LogLevel Logger::currentLevel = LogLevel::LOG_DEBUG;

void Logger::setLogLevel(LogLevel level) {
  currentLevel = level;
}

LogLevel Logger::getLogLevel() {
  return currentLevel;
}

void Logger::debug(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_DEBUG)
    return;
  va_list args;
  va_start(args, fmt);
  Serial.print("[DEBUG] ");
  Serial.vprintf(fmt, args);
  Serial.println();
  va_end(args);
}

void Logger::info(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_INFO)
    return;
  va_list args;
  va_start(args, fmt);
  Serial.print("[INFO] ");
  Serial.vprintf(fmt, args);
  Serial.println();
  va_end(args);
}

void Logger::warn(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_WARN)
    return;
  va_list args;
  va_start(args, fmt);
  Serial.print("[WARN] ");
  Serial.vprintf(fmt, args);
  Serial.println();
  va_end(args);
}

void Logger::error(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_ERROR)
    return;
  va_list args;
  va_start(args, fmt);
  Serial.print("[ERROR] ");
  Serial.vprintf(fmt, args);
  Serial.println();
  va_end(args);
}

void Logger::logln(const char* tag, const String& message, LogLevel level) {
  if (currentLevel > level)
    return;
  Serial.print("[");
  Serial.print(tag);
  Serial.print("] ");
  Serial.println(message);
}

void Logger::log(const char* tag, const String& message, LogLevel level) {
  if (currentLevel > level)
    return;
  Serial.print("[");
  Serial.print(tag);
  Serial.print("] ");
  Serial.print(message);
}

} // namespace utils
} // namespace lattice
