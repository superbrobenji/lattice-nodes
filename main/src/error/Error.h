#ifndef PLANETOPA_ERROR_H
#define PLANETOPA_ERROR_H

#include "ErrorCodes.h"
#include "ErrorCore.h"
#include "../core/Logger.h"
#include <esp_err.h>
#include <cstdint>

namespace planetopia {
namespace err {

// Helper: map legacy ErrorType to ErrorTypeDigit
inline ::planetopia::core::ErrorTypeDigit toDigit(utils::ErrorType t) {
  using namespace planetopia::core;
  switch (t) {
    case utils::ErrorType::GENERIC: return ErrorTypeDigit::GENERIC;
    case utils::ErrorType::SENSOR_FAIL: return ErrorTypeDigit::SENSOR;
    case utils::ErrorType::COMMUNICATION_FAIL: return ErrorTypeDigit::COMM;
    case utils::ErrorType::MEMORY_ERROR: return ErrorTypeDigit::MEMORY;
    case utils::ErrorType::CONFIG_ERROR: return ErrorTypeDigit::CONFIG;
    case utils::ErrorType::HARDWARE_FAILURE: return ErrorTypeDigit::HARDWARE;
    default: return ErrorTypeDigit::GENERIC;
  }
}

// Primary fail overload using digit components
inline bool fail(::planetopia::core::ErrorTypeDigit t, ::planetopia::core::ModuleDigit m, uint8_t sub, const char* msg) {
  utils::Logger::logln("ERROR", msg, utils::LogLevel::LOG_ERROR);
  utils::ErrorCore::getInstance().signalError(t, m, sub, msg);
  return false;
}

// Legacy fail – keeps compatibility while showing a generic error code (sub-code 0)
inline bool fail(utils::ErrorType type, const char* msg) {
  return fail(toDigit(type), ::planetopia::core::ModuleDigit::CORE, 0, msg);
}
[[noreturn]] inline void fatal(::planetopia::core::ErrorTypeDigit t, ::planetopia::core::ModuleDigit m, uint8_t sub, const char* msg) {
  utils::Logger::logln("FATAL", msg, utils::LogLevel::LOG_ERROR);
  utils::ErrorCore::getInstance().signalError(t, m, sub, msg);
  while (true) {}
}
[[noreturn]] inline void fatal(utils::ErrorType type, const char* msg) {
  fatal(toDigit(type), ::planetopia::core::ModuleDigit::CORE, 0, msg);
}
inline bool check(bool condition, utils::ErrorType type, const char* msg) {
  return condition ? true : fail(type, msg);
}
inline bool checkEsp(esp_err_t status, utils::ErrorType type, const char* msg) {
  if (status == ESP_OK) return true;
  utils::Logger::logln("ESP", String(msg) + ": " + esp_err_to_name(status), utils::LogLevel::LOG_ERROR);
  return fail(type, msg);
}
}
}

template<typename... Args>
inline bool ERROR_ASSERT(bool cond, const char* msg) {
  return planetopia::err::check(cond, planetopia::utils::ErrorType::CONFIG_ERROR, msg);
}

template<typename T>
inline bool ERROR_CHECK(bool cond, T t, const char* msg) {
  return planetopia::err::check(cond, t, msg);
}

template<typename T>
inline bool ERROR_CHECK_ESP_OK(esp_err_t expr, T t, const char* msg) {
  return planetopia::err::checkEsp(expr, t, msg);
}

#endif
