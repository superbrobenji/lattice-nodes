#ifndef GPIO_IN
#define GPIO_IN

#include <cstdint>

namespace lattice {
namespace hardware {

class GpioInput {
public:
  explicit GpioInput(uint8_t pin);
  ~GpioInput() = default;

  // Not virtual (post-Phase-G audit item I): every call site invokes init()
  // on a concrete type (Pir::init(), Button::init(), each calling
  // GpioInput::init() explicitly for shared validation) — never dispatched
  // through a GpioInput* / GpioInput& base pointer, so the vtable slot buys
  // nothing. ~GpioInput() is no longer virtual either (finding 12) — same
  // reasoning applies: it's never destroyed through a base pointer.
  bool init();
  static bool isValidInputPin(uint8_t pin);
  bool isInitialized() const;

protected:
  uint8_t _pin;
  bool _initialized;
};

} // namespace hardware
} // namespace lattice

#endif
