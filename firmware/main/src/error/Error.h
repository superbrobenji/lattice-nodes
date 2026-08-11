#ifndef PLANETOPA_ERROR_H
#define PLANETOPA_ERROR_H

#include "ErrorCodes.h"
#include "ErrorCore.h"
#include "../logging/Logger.h"
#include <esp_err.h>
#include <cstdint>
#include <cstdio>
#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#ifdef UNIT_TEST
#include <stdexcept>
extern int lattice_test_errFailCount;
#endif

namespace lattice {
namespace err {

#ifdef UNIT_TEST
class FatalError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
#endif

// Primary fail overload using digit components
inline bool fail(::lattice::core::ErrorTypeDigit t, ::lattice::core::ModuleDigit m, uint8_t sub,
                 const char* msg) {
#ifdef UNIT_TEST
  ++::lattice_test_errFailCount;
#endif
  LATTICE_LOGLN("ERROR", msg, utils::LogLevel::LOG_ERROR);
  err_core::signalError(t, m, sub, msg);
  return false;
}

[[noreturn]] inline void fatal(::lattice::core::ErrorTypeDigit t, ::lattice::core::ModuleDigit m,
                               uint8_t sub, const char* msg) {
  LATTICE_LOGLN("FATAL", msg, utils::LogLevel::LOG_ERROR);
  err_core::signalError(t, m, sub, msg);
#ifdef UNIT_TEST
  throw FatalError(msg ? msg : "fatal");
#else
  // Phase I Task 7 (WW): signalError() above now only ARMS the error LED's
  // blink pattern via Led::pulse() (non-blocking) — err_core::tick() must be
  // pumped here or the halted device would just show a solid-ON LED instead
  // of the intended blink pattern.
  while (true) {
    err_core::tick();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
#endif
}

// Finding 8: map legacy ErrorType to ErrorTypeDigit. The public two-arg
// fail(utils::ErrorType, msg)/fatal(utils::ErrorType, msg) convenience
// overloads this once fed are gone (migrated to the digit-based fail() at
// their only 2 call sites, in main.cpp's initHardwareOutputs()) — this
// helper now exists solely so check()/checkEsp() below, whose signatures
// callers outside this task's scope (MeshTransport.cpp, PeerRegistry.cpp)
// still depend on, can keep reporting through the single digit-based
// fail() as a generic error (ModuleDigit::CORE, sub-code 0).
inline ::lattice::core::ErrorTypeDigit toDigit(utils::ErrorType t) {
  using namespace lattice::core;
  switch (t) {
  case utils::ErrorType::GENERIC:
    return ErrorTypeDigit::GENERIC;
  case utils::ErrorType::SENSOR_FAIL:
    return ErrorTypeDigit::SENSOR;
  case utils::ErrorType::COMMUNICATION_FAIL:
    return ErrorTypeDigit::COMM;
  case utils::ErrorType::MEMORY_ERROR:
    return ErrorTypeDigit::MEMORY;
  case utils::ErrorType::CONFIG_ERROR:
    return ErrorTypeDigit::CONFIG;
  case utils::ErrorType::HARDWARE_FAILURE:
    return ErrorTypeDigit::HARDWARE;
  default:
    return ErrorTypeDigit::GENERIC;
  }
}
inline bool check(bool condition, utils::ErrorType type, const char* msg) {
  return condition ? true : fail(toDigit(type), ::lattice::core::ModuleDigit::CORE, 0, msg);
}
inline bool checkEsp(esp_err_t status, utils::ErrorType type, const char* msg) {
  if (status == ESP_OK)
    return true;
  LATTICE_LOGF("ESP", utils::LogLevel::LOG_ERROR, "%s: %s", msg ? msg : "",
               esp_err_to_name(status));
  return fail(toDigit(type), ::lattice::core::ModuleDigit::CORE, 0, msg);
}
} // namespace err
} // namespace lattice

template <typename T> inline bool ERROR_CHECK_ESP_OK(esp_err_t expr, T t, const char* msg) {
  return lattice::err::checkEsp(expr, t, msg);
}

#endif
