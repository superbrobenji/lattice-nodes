#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

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

}
}
#endif
