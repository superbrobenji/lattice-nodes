#include "Logger.h"
#include <cstdio>
#include <cstring>

namespace lattice {
namespace utils {

LogLevel Logger::currentLevel = LogLevel::LOG_DEBUG;

namespace {
void uartWrite(const char* s) {
  uart_write_bytes(UART_NUM_0, s, strlen(s));
}

void uartWriteLine(const char* tag, const char* prefix, const char* message) {
  uartWrite("[");
  uartWrite(tag);
  uartWrite("] ");
  if (prefix) {
    uartWrite(prefix);
  }
  uartWrite(message);
  // arduino-esp32's Print::println() emits "\r\n" (see
  // Print.cpp:168-170, `println(void) { return print("\r\n"); }`), which is
  // what the old Serial.println(message) call in logln() actually sent —
  // matched here byte-for-byte for the server/log-consumer side.
  uartWrite("\r\n");
}

// Mirrors the old Serial.print(prefix); Serial.vprintf(fmt, args); Serial.println();
// sequence — formats into a stack buffer (matches LATTICE_LOGF's existing
// 128-byte convention in Logger.h) then writes it as one native UART call.
//
// Note: the old Serial.vprintf() (arduino-esp32) used a 64-byte stack buffer
// but fell back to a dynamically malloc'd buffer sized to fit the full output
// on overflow, so it had no effective length cap. This fixed 128-byte buffer
// has no such fallback — formatted messages at or beyond 128 bytes are
// silently truncated by vsnprintf. That's a deliberate tradeoff matching
// LATTICE_LOGF's existing 128-byte convention (see Logger.h), not an
// oversight; revisit if a call site ever needs longer formatted output.
void uartWriteFormatted(const char* prefix, const char* fmt, va_list args) {
  char buf[128];
  vsnprintf(buf, sizeof(buf), fmt, args);
  uartWrite(prefix);
  uartWrite(buf);
  uartWrite("\r\n");
}
} // namespace

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
  uartWriteFormatted("[DEBUG] ", fmt, args);
  va_end(args);
}

void Logger::info(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_INFO)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[INFO] ", fmt, args);
  va_end(args);
}

void Logger::warn(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_WARN)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[WARN] ", fmt, args);
  va_end(args);
}

void Logger::error(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_ERROR)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[ERROR] ", fmt, args);
  va_end(args);
}

void Logger::logln(const char* tag, const char* message, LogLevel level) {
  if (currentLevel > level)
    return;
  uartWriteLine(tag, nullptr, message);
}

void Logger::log(const char* tag, const char* message, LogLevel level) {
  if (currentLevel > level)
    return;
  uartWrite("[");
  uartWrite(tag);
  uartWrite("] ");
  uartWrite(message);
}

} // namespace utils
} // namespace lattice
