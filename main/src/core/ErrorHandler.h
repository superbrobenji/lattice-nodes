#ifndef ERRORHANDLER_H
#define ERRORHANDLER_H

#include "src/hardware/output/Led.h"
#include "src/hardware/output/SevenSegDisplay.h"
#include "ErrorCodes.h"

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

class ErrorHandler {
public:
  static ErrorHandler& getInstance();

  void init(planetopia::hardware::Led* errorLed, planetopia::hardware::SevenSegDisplay* display = nullptr);

  // New verbose error signalling
  void signalError(core::ErrorTypeDigit type,
                   core::ModuleDigit module,
                   uint8_t subCode,
                   const char* message = nullptr);

  void signalError(ErrorType errorType, const char* message = nullptr);

  // Prevent copying
  ErrorHandler(const ErrorHandler&) = delete;
  ErrorHandler& operator=(const ErrorHandler&) = delete;

private:
  ErrorHandler();
  planetopia::hardware::Led* _errorLed;
  planetopia::hardware::SevenSegDisplay* _display;
  bool _initialized;

  void blinkPattern(ErrorType errorType);
  bool shouldRestart(ErrorType errorType) const;
  void restartDevice();
};

}  // namespace utils
}  // namespace planetopia
#endif
