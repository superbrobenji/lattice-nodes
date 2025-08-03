#include "ErrorHandler.h"
#include "Logger.h"
#include <Arduino.h>
#include <esp_system.h>

namespace planetopia {
namespace utils {

ErrorHandler::ErrorHandler()
  : _errorLed(nullptr), _initialized(false) {}

ErrorHandler& ErrorHandler::getInstance() {
  static ErrorHandler instance;
  return instance;
}

void ErrorHandler::init(hardware::Led* errorLed) {
  _errorLed = errorLed;
  _initialized = (_errorLed != nullptr);

  auto reason = esp_reset_reason();
  if (reason != ESP_RST_POWERON && reason != ESP_RST_SW && reason != ESP_RST_EXT) {
    // Device did NOT boot from normal power-on or clean restart.
    planetopia::utils::ErrorHandler::getInstance().signalError(
      planetopia::utils::ErrorType::HARDWARE_FAILURE,
      ("System recovered from unexpected reset, reason: " + String(reason)).c_str());
  }
  Logger::logln("ErrorHandler", "Initialized with error LED", LogLevel::LOG_INFO);
}

void ErrorHandler::signalError(ErrorType errorType, const char* message) {
  if (_initialized && _errorLed) {
    blinkPattern(errorType);
  }
  if (message) {
    Logger::logln("ErrorHandler", String("ERROR: ") + message, LogLevel::LOG_ERROR);
  }

  // If it's a severe error, attempt to restart
  if (shouldRestart(errorType)) {
    Logger::logln("ErrorHandler", "System will attempt to restart due to severe error.", LogLevel::LOG_ERROR);
    restartDevice();
  }
}

void ErrorHandler::blinkPattern(ErrorType errorType) {
  int blinks = 1;
  unsigned int onTime = 200;
  unsigned int offTime = 200;

  switch (errorType) {
    case ErrorType::GENERIC: blinks = 1; break;
    case ErrorType::SENSOR_FAIL: blinks = 2; break;
    case ErrorType::COMMUNICATION_FAIL: blinks = 3; break;
    case ErrorType::MEMORY_ERROR: blinks = 4; break;
    case ErrorType::CONFIG_ERROR: blinks = 5; break;
    case ErrorType::HARDWARE_FAILURE: blinks = 6; break;
    case ErrorType::USER_ERROR: blinks = 7; break;
    case ErrorType::TIMEOUT_ERROR: blinks = 8; break;
    default: blinks = 1; break;
  }
  if (_errorLed && _errorLed->isInitialized()) {
    _errorLed->blink(blinks, onTime, offTime);
    Logger::logln("ErrorHandler", String("Error LED blinked with pattern: ") + String(blinks), LogLevel::LOG_DEBUG);
  }
}

bool ErrorHandler::shouldRestart(ErrorType errorType) const {
  switch (errorType) {
    case ErrorType::COMMUNICATION_FAIL:
    case ErrorType::MEMORY_ERROR:
    case ErrorType::HARDWARE_FAILURE:
      return true;
    default:
      return false;
  }
}

void ErrorHandler::restartDevice() {
  delay(500);  // Short delay for LED blink to be seen
  Logger::logln("ErrorHandler", "Restarting device...", LogLevel::LOG_WARN);
  ESP.restart();
}

}  // namespace utils
}  // namespace planetopia
