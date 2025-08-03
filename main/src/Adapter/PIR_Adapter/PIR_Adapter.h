#ifndef PIR_ADAPTER_H
#define PIR_ADAPTER_H

#include "src/Adapter/Adapter.h"
#include "src/hardware/input/Pir.h"

namespace planetopia {
namespace adapter {

class PIR_Adapter : public Adapter {
public:
  explicit PIR_Adapter(int pin);
  bool init() override;
  void loop() override;

  // Trampoline for interrupt (must be static):
  static void detectMotionTrampoline();
  static void sendDataTrampoline(adapter_types adapterType, uint8_t data[12]);

private:
  hardware::Pir _pir;
  unsigned int _cooldownSeconds;
  unsigned long _lastTrigger;
  bool _timerActive;
  bool _motionSent;
  bool _interruptEnabled;
  bool _initialized;

  static PIR_Adapter* instance;
  void detectMotion();
};

}  // namespace adapter
}  // namespace planetopia

#endif
