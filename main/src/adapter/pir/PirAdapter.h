#ifndef PIR_ADAPTER_H
#define PIR_ADAPTER_H

#include "src/adapter/Adapter.h"
#include "src/hardware/input/Pir.h"
#include <cstdint>

namespace lattice {
namespace adapter {

class PirAdapter : public Adapter {
public:
  explicit PirAdapter(int pin);
  bool init() override;
  void loop() override;
  void onMeshDataImpl(const lattice::mesh::mesh_message& message) override;

  // Trampoline for interrupt (must be static):
  static void detectMotionTrampoline();
  static void sendDataTrampoline(adapter_types adapterType, uint8_t* data);

  // Singleton accessor (used by SerialAdapter in SIMULATE_MODE)
  static PirAdapter* getInstance() { return instance; }

#if SIMULATE_MODE
  // Inject a fake PIR motion event (bypasses hardware interrupt)
  void simulateMotion();
#endif

private:
  hardware::Pir _pir;
  uint16_t _cooldownSeconds;
  uint32_t _lastTrigger;
  bool _timerActive;
  bool _motionSent;
  bool _interruptEnabled;
  bool _initialized;
  uint32_t _lastHealthMillis;

  static PirAdapter* instance;
  void detectMotion();
  static void sendNodeHealth();
};

} // namespace adapter
} // namespace lattice

#endif
