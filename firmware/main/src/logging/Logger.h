#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <cstdio>

#ifndef LATTICE_LOG_LEVEL
#define LATTICE_LOG_LEVEL 3 // 0=none 1=error 2=warn 3=info 4=debug
#endif

#if LATTICE_LOG_LEVEL >= 4
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

// ---------------------------------------------------------------------------------------------
// LATTICE_LOG / LATTICE_LOGLN — compile-time log-level gating (design doc §1, Phase G).
//
// Numeric values mirror lattice::utils::LogLevel exactly (DEBUG=0 .. NONE=4) so the #if below
// can compare against it directly, without needing the enum type available yet at this point in
// the header. When LATTICE_DEFAULT_LOG_LEVEL == LOG_NONE (the production default — see
// project_config.h §6 "Logging"), both macros fold to ((void)0): the tag/message/level arguments
// are never evaluated, so format strings and String concatenations at call sites never reach the
// translation unit's .rodata / heap. Otherwise they route to the existing runtime-dispatched
// Logger::log/logln (unchanged behaviour).
#define LATTICE_LOG_LEVEL_NONE 4

// project_config.h defines LATTICE_DEFAULT_LOG_LEVEL above its own #include of this header, so
// that value is authoritative when present. This fallback only covers translation units that
// include Logger.h directly without going through project_config.h first; it is pinned to
// LOG_NONE (the production default) so such a TU never silently gets logging turned on.
#ifndef LATTICE_DEFAULT_LOG_LEVEL
#define LATTICE_DEFAULT_LOG_LEVEL LATTICE_LOG_LEVEL_NONE
#endif

#if LATTICE_DEFAULT_LOG_LEVEL == LATTICE_LOG_LEVEL_NONE
#define LATTICE_LOG(tag, msg, level) ((void)0)
#define LATTICE_LOGLN(tag, msg, level) ((void)0)
#else
#define LATTICE_LOG(tag, msg, level) ::lattice::utils::Logger::log(tag, msg, level)
#define LATTICE_LOGLN(tag, msg, level) ::lattice::utils::Logger::logln(tag, msg, level)
#endif

namespace lattice {
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
} // namespace lattice
#endif
