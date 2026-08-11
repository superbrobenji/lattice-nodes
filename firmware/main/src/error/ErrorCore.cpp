#include "ErrorCore.h"
#include "Error.h"
#include "../logging/Logger.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <cstdio>
using lattice::core::ErrorTypeDigit;
using lattice::core::makeErrorCode;
using lattice::core::ModuleDigit;
using lattice::utils::ErrorType;

namespace lattice {
namespace err_core {

namespace {

detail::State _state;

void blinkPattern(ErrorType t) {
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
  if (_state.errorLed && _state.errorLed->isInitialized())
    _state.errorLed->pulse(b, 200, 200);
}

bool shouldRestart(ErrorType t) {
  return t == ErrorType::MEMORY_ERROR || t == ErrorType::HARDWARE_FAILURE;
}

[[noreturn]] void restartDevice() {
  LATTICE_LOGLN("ErrorCore", "Restarting device...", lattice::utils::LogLevel::LOG_WARN);
#ifdef UNIT_TEST
  throw lattice::err::FatalError("ErrorCore::restartDevice");
#else
  esp_restart();
#endif
}

} // namespace

void init(hardware::Led* led, hardware::SevenSegDisplay* display) {
  _state.errorLed = led;
  _state.display = display;
  _state.initialized = (_state.errorLed != nullptr);
  auto r = esp_reset_reason();
  if (r != ESP_RST_POWERON && r != ESP_RST_SW && r != ESP_RST_EXT) {
    signalError(ErrorTypeDigit::HARDWARE, ModuleDigit::CORE, 1, "Unexpected reset");
  }
  LATTICE_LOGLN("ErrorCore", "Initialized", lattice::utils::LogLevel::LOG_INFO);
}

void signalError(ErrorTypeDigit t, ModuleDigit m, uint8_t sub, const char* msg) {
  uint16_t code = makeErrorCode(t, m, sub);
  if (_state.display)
    _state.display->show(static_cast<int>(code));
  ErrorType lt = ErrorType::GENERIC;
  if (t == ErrorTypeDigit::HARDWARE)
    lt = ErrorType::HARDWARE_FAILURE;
  else if (t == ErrorTypeDigit::COMM)
    lt = ErrorType::COMMUNICATION_FAIL;
  else if (t == ErrorTypeDigit::MEMORY)
    lt = ErrorType::MEMORY_ERROR;
  else if (t == ErrorTypeDigit::CONFIG)
    lt = ErrorType::CONFIG_ERROR;
  else if (t == ErrorTypeDigit::CRYPTO)
    // AEAD/ECDH failures (e.g. Phase A epoch-rollback guard) are security-
    // critical and must halt the node, same as a hardware fault — there is no
    // safe way to continue running past a would-be AEAD nonce reuse.
    lt = ErrorType::HARDWARE_FAILURE;
  signalError(lt, msg);
}

void signalError(ErrorType type, const char* msg) {
  if (msg) {
    LATTICE_LOGF("Error", lattice::utils::LogLevel::LOG_ERROR, "ERROR: %s", msg);
  }
  if (_state.initialized && _state.errorLed) {
    if (_state.inCallbackContext) {
      // Defer blink to main loop — never block in callback context
      _state.pendingBlink = true;
      _state.pendingBlinkType = type;
    } else {
      blinkPattern(type);
    }
  }
  if (shouldRestart(type))
    restartDevice();
}

void setCallbackContext(bool inCallback) {
  _state.inCallbackContext = inCallback;
}

void drainPendingBlink() {
  if (_state.pendingBlink) {
    _state.pendingBlink = false;
    blinkPattern(_state.pendingBlinkType);
  }
}

void tick() {
  if (_state.errorLed) {
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    _state.errorLed->update(now);
  }
}

#ifdef UNIT_TEST
detail::State& debugStateForTest() {
  return _state;
}
#endif

} // namespace err_core
} // namespace lattice
