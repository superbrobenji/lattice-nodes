#include "PIR_Adapter.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

PIR_Adapter* PIR_Adapter::instance = nullptr;

PIR_Adapter::PIR_Adapter(int pin)
  : Adapter(pin),
    _pir(pin),
    _cooldownSeconds(3),
    _lastTrigger(0),
    _timerActive(false),
    _motionSent(false),
    _interruptEnabled(false),
    _initialized(false) {
  _adapterType = PIR_ADAPTER;
}

bool PIR_Adapter::init() {
    if (_initialized) {
        Logger::logln("PIR_Adapter", "Warning: Already initialized.", LogLevel::LOG_WARN);
        return true;
    }

    if (!_pir.init()) {
        ErrorHandler::getInstance().signalError(
            ErrorType::CONFIG_ERROR,
            "PIR_Adapter: PIR hardware failed to initialize."
        );
        return false;
    }

    instance = this;
    _pir.attachInterrupt(PIR_Adapter::detectMotionTrampoline, RISING);

    _interruptEnabled = true;
    _initialized = true;

    Logger::logln("PIR_Adapter", "Initialized successfully", LogLevel::LOG_INFO);
    return true;
}

void PIR_Adapter::detectMotionTrampoline() {
    if (instance) instance->detectMotion();
}

void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[12]) {
    if (instance) instance->sendDataThroughMesh(adapterType, data);
}

void PIR_Adapter::detectMotion() {
    if (!_interruptEnabled) return;
    _pir.signalMotion();
    _interruptEnabled = false;
    _pir.detachInterrupt();
}

void PIR_Adapter::loop() {
    if (!_initialized) return;

    unsigned long now = millis();

    if (_pir.isMotionDetected()) {
        _lastTrigger = now;
        _timerActive = true;
        _motionSent = false;
        _pir.clearMotion();
    }

    if (_timerActive && !_motionSent) {
        Logger::logln("PIR_Adapter", "MOTION DETECTED!", LogLevel::LOG_INFO);
        _motionSent = true;
        uint8_t data[12] = { 1 };
        PIR_Adapter::sendDataTrampoline(_adapterType, data);
    }

    if (_timerActive && (now - _lastTrigger > (_cooldownSeconds * 1000))) {
        Logger::logln("PIR_Adapter", "Cooldown ended. Re-arming sensor.", LogLevel::LOG_DEBUG);
        _timerActive = false;
        _motionSent = false;

        if (!_pir.attachInterrupt(PIR_Adapter::detectMotionTrampoline, RISING)) {
            ErrorHandler::getInstance().signalError(
                ErrorType::HARDWARE_FAILURE,
                "PIR_Adapter: Could not re-attach interrupt (possible hardware error)"
            );
            return;
        }
        _interruptEnabled = true;
    }
}

} // namespace adapter
} // namespace planetopia
