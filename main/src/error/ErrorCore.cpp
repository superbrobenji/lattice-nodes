#include "ErrorCore.h"
#include "../core/Logger.h"
#include <esp_system.h>
using planetopia::core::ErrorTypeDigit;
using planetopia::core::makeErrorCode;
using planetopia::core::ModuleDigit;
namespace planetopia {
namespace utils {
ErrorCore::ErrorCore()
    : _errorLed(nullptr), _display(nullptr), _initialized(false), _pendingBlink(false),
      _pendingBlinkType(ErrorType::GENERIC), _inCallbackContext(false) {}
ErrorCore& ErrorCore::getInstance() {
  static ErrorCore i;
  return i;
}
void ErrorCore::init(hardware::Led* led, hardware::SevenSegDisplay* display) {
  _errorLed = led;
  _display = display;
  _initialized = (_errorLed != nullptr);
  auto r = esp_reset_reason();
  if (r != ESP_RST_POWERON && r != ESP_RST_SW && r != ESP_RST_EXT) {
    signalError(ErrorTypeDigit::HARDWARE, ModuleDigit::CORE, 1, "Unexpected reset");
  }
  Logger::logln("ErrorCore", "Initialized", LogLevel::LOG_INFO);
}
void ErrorCore::signalError(ErrorTypeDigit t, ModuleDigit m, uint8_t sub, const char* msg) {
  uint16_t code = makeErrorCode(t, m, sub);
  if (_display)
    _display->show(static_cast<int>(code));
  ErrorType lt = ErrorType::GENERIC;
  if (t == ErrorTypeDigit::HARDWARE)
    lt = ErrorType::HARDWARE_FAILURE;
  else if (t == ErrorTypeDigit::COMM)
    lt = ErrorType::COMMUNICATION_FAIL;
  else if (t == ErrorTypeDigit::MEMORY)
    lt = ErrorType::MEMORY_ERROR;
  else if (t == ErrorTypeDigit::CONFIG)
    lt = ErrorType::CONFIG_ERROR;
  signalError(lt, msg);
}
void ErrorCore::signalError(ErrorType type, const char* msg) {
  if (msg)
    Logger::logln("Error", String("ERROR: ") + msg, LogLevel::LOG_ERROR);
  if (_initialized && _errorLed) {
    if (_inCallbackContext) {
      // Defer blink to main loop — never block in callback context
      _pendingBlink = true;
      _pendingBlinkType = type;
    } else {
      blinkPattern(type);
    }
  }
  if (shouldRestart(type))
    restartDevice();
}

void ErrorCore::drainPendingBlink() {
  if (_pendingBlink) {
    _pendingBlink = false;
    blinkPattern(_pendingBlinkType);
  }
}
void ErrorCore::blinkPattern(ErrorType t) {
  int b = 1;
  switch (t) {
  case ErrorType::GENERIC:
    b = 1;
    break;
  case ErrorType::SENSOR_FAIL:
    b = 2;
    break;
  case ErrorType::COMMUNICATION_FAIL:
    b = 3;
    break;
  case ErrorType::MEMORY_ERROR:
    b = 4;
    break;
  case ErrorType::CONFIG_ERROR:
    b = 5;
    break;
  case ErrorType::HARDWARE_FAILURE:
    b = 6;
    break;
  case ErrorType::USER_ERROR:
    b = 7;
    break;
  case ErrorType::TIMEOUT_ERROR:
    b = 8;
    break;
  default:
    b = 1;
  }
  if (_errorLed && _errorLed->isInitialized())
    _errorLed->blink(b, 200, 200);
}
bool ErrorCore::shouldRestart(ErrorType t) const {
  return t == ErrorType::MEMORY_ERROR || t == ErrorType::HARDWARE_FAILURE;
}
[[noreturn]] void ErrorCore::restartDevice() {
  Logger::logln("ErrorCore", "Restarting device...", LogLevel::LOG_WARN);
  ESP.restart();
}
} // namespace utils
} // namespace planetopia
