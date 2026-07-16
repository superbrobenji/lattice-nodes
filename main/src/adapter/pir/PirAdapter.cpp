#include "PirAdapter.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include <cstdint>
#include <cstring>
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/AdapterFactory.h"
#include <esp_wifi.h>

namespace lattice {
namespace adapter {

using namespace lattice::utils;

PirAdapter* PirAdapter::instance = nullptr;

PirAdapter::PirAdapter(int pin)
    : Adapter(pin), _pir(pin), _cooldownSeconds(3), _lastTrigger(0), _timerActive(false),
      _motionSent(false), _interruptEnabled(false), _initialized(false), _lastHealthMillis(0) {
  _adapterType = adapter_types::PIR_ADAPTER;
}

bool PirAdapter::init() {
  if (_initialized) {
    Logger::logln("PIR_Adapter", "Warning: Already initialized.", LogLevel::LOG_WARN);
    return true;
  }

  if (!_pir.init()) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                       1, "PIR_Adapter: PIR hardware failed to initialize.");
    return false;
  }

  instance = this;
  if (!_pir.attachInterrupt(PirAdapter::detectMotionTrampoline, RISING)) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                       2, "PIR_Adapter: Failed to attach interrupt.");
    return false;
  }

  _interruptEnabled = true;
  _initialized = true;

  Logger::logln("PIR_Adapter", "Initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void PirAdapter::detectMotionTrampoline() {
  if (instance)
    instance->detectMotion();
}

void PirAdapter::sendDataTrampoline(adapter_types adapterType, uint8_t* data) {
  if (instance)
    instance->sendDataThroughMesh(adapterType, data);
}

void PirAdapter::detectMotion() {
  if (!_interruptEnabled)
    return;
  _pir.signalMotion();
  _interruptEnabled = false;
  _pir.detachInterrupt();
}

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

void PirAdapter::sendNodeHealth() {
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

void PirAdapter::loop() {
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
    PirAdapter::sendDataTrampoline(_adapterType, data);
  }

  if (_timerActive && (now - _lastTrigger > (_cooldownSeconds * 1000U))) {
    Logger::logln("PIR_Adapter", "Cooldown ended. Re-arming sensor.", LogLevel::LOG_DEBUG);
    _timerActive = false;
    _motionSent = false;

    if (!_pir.attachInterrupt(PirAdapter::detectMotionTrampoline, RISING)) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE,
                         lattice::core::ModuleDigit::ADAPTER, 2,
                         "PIR_Adapter: Could not re-attach interrupt (possible hardware error)");
      return;
    }
    _interruptEnabled = true;
  }

  if (now - _lastHealthMillis >= lattice::config::HEALTH_REPORT_INTERVAL_MS) {
    _lastHealthMillis = now;
    sendNodeHealth();
  }
}

void PirAdapter::onMeshDataImpl(const lattice::mesh::mesh_message& /*message*/) {
  // No-op for PIR: currently nothing to do on inbound messages of this type
}

#if SIMULATE_MODE
void PirAdapter::simulateMotion() {
  // Drive the SAME path the hardware interrupt would (detectMotionTrampoline ->
  // detectMotion) instead of poking the sensor directly. detectMotion() honours
  // the _interruptEnabled gate and detaches the interrupt -- which is precisely
  // how the device enforces its post-trigger cooldown: while _timerActive, the
  // interrupt stays detached (re-armed only by loop() once _cooldownSeconds has
  // elapsed), so a real PIR firing again mid-cooldown is physically ignored.
  // Calling _pir.signalMotion() directly bypassed that gate and let a simulated
  // re-trigger inject motion the hardware never could, masking the cooldown.
  Logger::logln("PIR_Adapter", "SIM: Injecting fake PIR motion event", LogLevel::LOG_WARN);
  detectMotion();
}
#endif

} // namespace adapter
} // namespace lattice
