#include "PIR_Adapter.h"
#include "lib/planetopia-protocol/c/opcodes.h"
#include <cstdint>
#include <cstring>
#include "src/core/Logger.h"
#include "src/error/Error.h"
#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include <esp_wifi.h>

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

PIR_Adapter* PIR_Adapter::instance = nullptr;

PIR_Adapter::PIR_Adapter(int pin)
    : Adapter(pin), _pir(pin), _cooldownSeconds(3), _lastTrigger(0), _timerActive(false),
      _motionSent(false), _interruptEnabled(false), _initialized(false), _lastHealthMillis(0) {
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

void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[64]) {
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

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

void PIR_Adapter::sendNodeHealth() {
  uint8_t data[64] = {0};
  data[0] = OP_NODE_HEALTH;
  data[1] = AdapterFactory::adapterTypeToEEPROM(adapter_types::PIR_ADAPTER);
  uint8_t mac[6];
  readOwnMac(mac);
  memcpy(&data[2], mac, 6);
  uint32_t uptimeSec = millis() / 1000;
  data[8] = static_cast<uint8_t>(uptimeSec & 0xFF);
  data[9] = static_cast<uint8_t>((uptimeSec >> 8) & 0xFF);
  data[10] = static_cast<uint8_t>((uptimeSec >> 16) & 0xFF);
  data[11] = static_cast<uint8_t>((uptimeSec >> 24) & 0xFF);
  if (instance)
    instance->sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, data);
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
    uint8_t data[64] = {1};
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

  if (now - _lastHealthMillis >= planetopia::config::HEALTH_REPORT_INTERVAL_MS) {
    _lastHealthMillis = now;
    sendNodeHealth();
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
