#ifndef ERRORCORE_H
#define ERRORCORE_H

#include "../hardware/output/Led.h"
#include "../hardware/output/SevenSegDisplay.h"
#include "ErrorCodes.h"
#include "../core/Logger.h"
#include <Arduino.h>
namespace planetopia {
namespace utils {
enum class ErrorType : uint8_t {
  GENERIC = 0,
  SENSOR_FAIL = 1,
  COMMUNICATION_FAIL = 2,
  MEMORY_ERROR = 3,
  CONFIG_ERROR = 4,
  HARDWARE_FAILURE = 5,
  USER_ERROR = 6,
  TIMEOUT_ERROR = 7
};
class ErrorCore {
public:
  static ErrorCore& getInstance();
  void init(planetopia::hardware::Led* led,
            planetopia::hardware::SevenSegDisplay* display = nullptr);
  void signalError(core::ErrorTypeDigit t, core::ModuleDigit m, uint8_t sub,
                   const char* msg = nullptr);
  void signalError(ErrorType type, const char* msg = nullptr);
  void setCallbackContext(bool inCallback) { _inCallbackContext = inCallback; }
  void drainPendingBlink(); // Call from main loop
  ErrorCore(const ErrorCore&) = delete;
  ErrorCore& operator=(const ErrorCore&) = delete;

private:
  ErrorCore();
  planetopia::hardware::Led* _errorLed;
  planetopia::hardware::SevenSegDisplay* _display;
  bool _initialized;
  volatile bool _pendingBlink;
  ErrorType _pendingBlinkType;
  bool _inCallbackContext; // set true when inside ESP-NOW recv task
  void blinkPattern(ErrorType);
  bool shouldRestart(ErrorType) const;
  [[noreturn]] void restartDevice();
};
} // namespace utils
} // namespace planetopia
#endif
