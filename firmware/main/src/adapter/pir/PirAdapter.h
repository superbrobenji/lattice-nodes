#ifndef PIR_ADAPTER_H
#define PIR_ADAPTER_H

#include "src/adapter/Adapter.h"
#include "src/hardware/input/Pir.h"
#include <cstdint>
#include <esp_attr.h>

namespace lattice {
namespace adapter {

class PirAdapter : public Adapter {
public:
  explicit PirAdapter(uint8_t pin);
  bool init() override;
  void loop() override;
  void onMeshDataImpl(const lattice::mesh::mesh_message& message) override;

  // Trampoline for interrupt (must be static + IRAM_ATTR):
  static void IRAM_ATTR detectMotionTrampoline();
  static void sendDataTrampoline(adapter_types adapterType, uint8_t* data);

  // Singleton accessor (used by SerialAdapter in SIMULATE_MODE)
  static PirAdapter* getInstance() { return instance; }

#if SIMULATE_MODE
  // Inject a fake PIR motion event (bypasses hardware interrupt)
  void simulateMotion();
#endif

private:
  // Phase G audit item K: motion-cooldown state collapsed from two bools
  // (_timerActive/_motionSent — only 3 of their 4 combinations were ever
  // reachable) into one 3-state enum.
  //   IDLE         == old (timerActive=false, motionSent=false)
  //   PENDING_SEND == old (timerActive=true,  motionSent=false)
  //   COOLDOWN     == old (timerActive=true,  motionSent=true)
  enum class PirState : uint8_t { IDLE, PENDING_SEND, COOLDOWN };

  hardware::Pir _pir;
  // Never configured differently at runtime — was a per-instance member,
  // now a compile-time constant (Phase G audit item K).
  static constexpr uint16_t _cooldownSeconds = 3;
  // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
  // millis() -> esp_timer_get_time()/1000ULL swap.
  uint64_t _lastTrigger;
  PirState _state{PirState::IDLE};
  bool _interruptEnabled;
  bool _initialized;
  // Phase H2 item W: the health-report interval timer moved to the shared
  // Adapter base (_lastHealthMillis there, driven via healthTickDue()).

#ifdef UNIT_TEST
public:
#else
private:
#endif
  static PirAdapter* instance;

private:
  void detectMotion();
  static void sendNodeHealth();
};

} // namespace adapter
} // namespace lattice

#endif
