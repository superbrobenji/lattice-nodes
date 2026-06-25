#ifndef PIR_ADAPTER_H
#define PIR_ADAPTER_H

#include "src/Adapter/Adapter.h"
#include "src/hardware/input/Pir.h"
#include <cstdint>

namespace planetopia {
namespace adapter {

class PIR_Adapter : public Adapter {
public:
  explicit PIR_Adapter(int pin);
  bool init() override;
  void loop() override;
  void onMeshDataImpl(const planetopia::mesh::mesh_message& message) override;

  // Trampoline for interrupt (must be static):
  static void detectMotionTrampoline();
  static void sendDataTrampoline(adapter_types adapterType, uint8_t data[12]);

  // Singleton accessor (used by Serial_Adapter in SIMULATE_MODE)
  static PIR_Adapter* getInstance() { return instance; }

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

  static PIR_Adapter* instance;
  void detectMotion();
};

} // namespace adapter
} // namespace planetopia

#endif
