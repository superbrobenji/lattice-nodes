#ifndef GPIO_OUT
#define GPIO_OUT

#include <cstdint>

namespace lattice {
namespace hardware {

class GpioOutput {
public:
  explicit GpioOutput(uint8_t pin);
  ~GpioOutput() = default;

  // Not virtual (post-Phase-G audit item I) — see GpioInput::init()'s comment;
  // same reasoning: never dispatched through a GpioOutput* base pointer.
  // ~GpioOutput() is no longer virtual either (finding 12), same reasoning.
  bool init();
  static bool isValidOutputPin(uint8_t pin);
  bool isInitialized() const;

protected:
  uint8_t _pin;
  bool _initialized;
};

} // namespace hardware
} // namespace lattice

#endif
