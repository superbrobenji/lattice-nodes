#include "PIR_Adapter.h"
#include <cstdint>
#include "src/core/Logger.h"
#include "src/error/Error.h"
#include "src/Mesh/Mesh.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

PIR_Adapter* PIR_Adapter::instance = nullptr;

PIR_Adapter::PIR_Adapter(int pin)
    : Adapter(pin), _pir(pin), _cooldownSeconds(3), _lastTrigger(0), _timerActive(false),
      _motionSent(false), _interruptEnabled(false), _initialized(false) {
  _adapterType = adapter_types::PIR_ADAPTER;
}

bool PIR_Adapter::init() {
  if (_initialized) {
    Logger::logln("PIR_Adapter", "Warning: Already initialized.", LogLevel::LOG_WARN);
    return true;
  }

  if (!_pir.init()) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                          planetopia::core::ModuleDigit::ADAPTER, 1,
                          "PIR_Adapter: PIR hardware failed to initialize.");
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
  if (instance)
    instance->detectMotion();
}

void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[12]) {
  if (instance)
    instance->sendDataThroughMesh(adapterType, data);
}

void PIR_Adapter::detectMotion() {
  if (!_interruptEnabled)
    return;
  _pir.signalMotion();
  _interruptEnabled = false;
  _pir.detachInterrupt();
}

void PIR_Adapter::loop() {
  if (!_initialized)
    return;

  uint32_t now = millis();

  if (_pir.isMotionDetected()) {
    _lastTrigger = now;
    _timerActive = true;
    _motionSent = false;
    _pir.clearMotion();
  }

  if (_timerActive && !_motionSent) {
    Logger::logln("PIR_Adapter", "MOTION DETECTED!", LogLevel::LOG_INFO);
    _motionSent = true;
    uint8_t data[12] = {1};
    PIR_Adapter::sendDataTrampoline(_adapterType, data);
  }

  if (_timerActive && (now - _lastTrigger > (_cooldownSeconds * 1000U))) {
    Logger::logln("PIR_Adapter", "Cooldown ended. Re-arming sensor.", LogLevel::LOG_DEBUG);
    _timerActive = false;
    _motionSent = false;

    if (!_pir.attachInterrupt(PIR_Adapter::detectMotionTrampoline, RISING)) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::ADAPTER, 2,
                            "PIR_Adapter: Could not re-attach interrupt (possible hardware error)");
      return;
    }
    _interruptEnabled = true;
  }
}

void PIR_Adapter::onMeshDataImpl(const planetopia::mesh::mesh_message& /*message*/) {
  // No-op for PIR: currently nothing to do on inbound messages of this type
}

#if SIMULATE_MODE
void PIR_Adapter::simulateMotion() {
  // Directly signal motion without requiring hardware interrupt
  Logger::logln("PIR_Adapter", "SIM: Injecting fake PIR motion event", LogLevel::LOG_WARN);
  _pir.signalMotion();
}
#endif

} // namespace adapter
} // namespace planetopia
