#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <cstdio>

#ifndef PLANETOPIA_LOG_LEVEL
#define PLANETOPIA_LOG_LEVEL 3 // 0=none 1=error 2=warn 3=info 4=debug
#endif

#if PLANETOPIA_LOG_LEVEL >= 4
#define LOG_D(tag, fmt, ...)                                                                       \
  do {                                                                                             \
    char _buf[128];                                                                                \
    snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                                              \
    Logger::logln(tag, _buf, LogLevel::LOG_DEBUG);                                                 \
  } while (0)
#else
#define LOG_D(tag, fmt, ...)                                                                       \
  do {                                                                                             \
  } while (0)
#endif

namespace planetopia {
namespace utils {

enum class LogLevel : uint8_t {
  LOG_DEBUG = 0,
  LOG_INFO = 1,
  LOG_WARN = 2,
  LOG_ERROR = 3,
  LOG_NONE = 4
};

class Logger {
public:
  static void setLogLevel(LogLevel level);
  static LogLevel getLogLevel();

  static void debug(const char* fmt, ...);
  static void info(const char* fmt, ...);
  static void warn(const char* fmt, ...);
  static void error(const char* fmt, ...);

  static void logln(const char* tag, const String& message, LogLevel level = LogLevel::LOG_INFO);
  static void log(const char* tag, const String& message, LogLevel level = LogLevel::LOG_INFO);

private:
  static LogLevel currentLevel;
};

} // namespace utils
} // namespace planetopia
#endif
