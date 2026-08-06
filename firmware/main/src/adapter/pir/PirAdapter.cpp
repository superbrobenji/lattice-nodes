#include "PirAdapter.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include <cstdint>
#include <esp_timer.h>
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/mesh/Mesh.h"

namespace lattice {
namespace adapter {

using namespace lattice::utils;

PirAdapter* PirAdapter::instance = nullptr;

PirAdapter::PirAdapter(uint8_t pin)
    : Adapter(pin), _pir(pin), _lastTrigger(0), _state(PirState::IDLE), _interruptEnabled(false),
      _initialized(false) {
  _adapterType = adapter_types::PIR_ADAPTER;
}

bool PirAdapter::init() {
  if (_initialized) {
    LATTICE_LOGLN("PIR_Adapter", "Warning: Already initialized.", LogLevel::LOG_WARN);
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

  LATTICE_LOGLN("PIR_Adapter", "Initialized successfully", LogLevel::LOG_INFO);
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

// Phase H2 item W: byte layout now built once, in the shared base
// Adapter::buildHealthFrame(). PIR keeps its own send path (routes through
// the mesh via sendDataThroughMesh to reach the master) since — unlike
// SerialAdapter, which has a direct serial link — it has no self-originated
// transmit path.
void PirAdapter::sendNodeHealth() {
  if (!instance)
    return;
  uint8_t data[64] = {0};
  instance->buildHealthFrame(OP_NODE_HEALTH, data, sizeof(data));
  instance->sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, data);
}

void PirAdapter::loop() {
  if (!_initialized)
    return;

  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;

  if (_pir.isMotionDetected()) {
    _lastTrigger = now;
    _state = PirState::PENDING_SEND;
    _pir.clearMotion();
  }

  if (_state == PirState::PENDING_SEND) {
    LATTICE_LOGLN("PIR_Adapter", "MOTION DETECTED!", LogLevel::LOG_INFO);
    _state = PirState::COOLDOWN;
    uint8_t data[64] = {1};
    PirAdapter::sendDataTrampoline(_adapterType, data);
  }

  if (_state == PirState::COOLDOWN && (now - _lastTrigger > (_cooldownSeconds * 1000U))) {
    LATTICE_LOGLN("PIR_Adapter", "Cooldown ended. Re-arming sensor.", LogLevel::LOG_DEBUG);
    _state = PirState::IDLE;

    if (!_pir.attachInterrupt(PirAdapter::detectMotionTrampoline, RISING)) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE,
                         lattice::core::ModuleDigit::ADAPTER, 2,
                         "PIR_Adapter: Could not re-attach interrupt (possible hardware error)");
      return;
    }
    _interruptEnabled = true;
  }

  // Phase H2 item W: interval tick lives in the shared Adapter base.
  if (healthTickDue(now)) {
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
  // how the device enforces its post-trigger cooldown: while _state is
  // PENDING_SEND/COOLDOWN, the interrupt stays detached (re-armed only by
  // loop() once _cooldownSeconds has elapsed), so a real PIR firing again
  // mid-cooldown is physically ignored.
  // Calling _pir.signalMotion() directly bypassed that gate and let a simulated
  // re-trigger inject motion the hardware never could, masking the cooldown.
  LATTICE_LOGLN("PIR_Adapter", "SIM: Injecting fake PIR motion event", LogLevel::LOG_WARN);
  detectMotion();
}
#endif

} // namespace adapter
} // namespace lattice
