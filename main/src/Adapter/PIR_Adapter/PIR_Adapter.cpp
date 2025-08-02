#include "PIR_Adapter.h"
#include "src/utils/Logger.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

PIR_Adapter* PIR_Adapter::instance = nullptr;

PIR_Adapter::PIR_Adapter(int pin)
  : Adapter(pin),
    _pin(pin),
    _cooldownSeconds(3),
    _lastTrigger(0),
    _timerActive(false),
    _motionSent(false),
    _interruptEnabled(false),
    _initialized(false),
    _motionDetected(false) {
  _adapterType = PIR_ADAPTER;
}

void PIR_Adapter::init() {
  if (_initialized) {
    Logger::logln("PIR_Adapter", "Warning: Already initialized.");
    return;
  }

  if (_pin < 0 || _pin > 39) {
    Logger::logln("PIR_Adapter", "Error: Invalid pin number.");
    return;
  }

  int irq = digitalPinToInterrupt(_pin);
  if (irq == NOT_AN_INTERRUPT) {
    Logger::logln("PIR_Adapter", "Error: Selected pin does not support interrupts.");
    return;
  }

  instance = this;
  pinMode(_pin, INPUT_PULLUP);
  attachInterrupt(irq, PIR_Adapter::detectMotionTrampoline, RISING);

  _interruptEnabled = true;
  _initialized = true;

  Logger::logln("PIR_Adapter", "Initialized successfully on pin " + String(_pin));
}

void PIR_Adapter::detectMotionTrampoline() {
  if (instance) instance->detectMotion();
}

void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[12]) {
  if (instance) instance->sendDataThroughMesh(adapterType, data);
}

void IRAM_ATTR PIR_Adapter::detectMotion() {
  if (!_interruptEnabled) return;

  _motionDetected = true;
  _interruptEnabled = false;
  detachInterrupt(digitalPinToInterrupt(_pin));
}

void PIR_Adapter::loop() {
  if (!_initialized) return;

  unsigned long now = millis();

  if (_motionDetected) {
    _lastTrigger = now;
    _timerActive = true;
    _motionSent = false;
    _motionDetected = false;
  }

  if (_timerActive && !_motionSent) {
    Logger::logln("PIR_Adapter", "MOTION DETECTED!");
    _motionSent = true;
    uint8_t data[12] = { 1 };
    PIR_Adapter::sendDataTrampoline(_adapterType, data);
  }

  if (_timerActive && (now - _lastTrigger > (_cooldownSeconds * 1000))) {
    Logger::logln("PIR_Adapter", "Cooldown ended. Re-arming sensor.");
    _timerActive = false;
    _motionSent = false;

    attachInterrupt(digitalPinToInterrupt(_pin), PIR_Adapter::detectMotionTrampoline, RISING);
    _interruptEnabled = true;
  }
}

}
}
