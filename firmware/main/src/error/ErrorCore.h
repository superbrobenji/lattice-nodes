#ifndef ERRORCORE_H
#define ERRORCORE_H

#include "../hardware/output/Led.h"
#include "../hardware/output/SevenSegDisplay.h"
#include "ErrorCodes.h"
#include "../logging/Logger.h"
#include <cstdint>
namespace lattice {
namespace utils {
enum class ErrorType : uint8_t {
  GENERIC = 0,
  SENSOR_FAIL = 1,
  COMMUNICATION_FAIL = 2,
  MEMORY_ERROR = 3,
  CONFIG_ERROR = 4,
  HARDWARE_FAILURE = 5,
  USER_ERROR = 6,
  TIMEOUT_ERROR = 7
};
} // namespace utils
} // namespace lattice

// Phase H2 item AA: Meyers singleton -> namespace of free functions backed by
// file-static state (ErrorCore.cpp). Named err_core (not err) to avoid
// colliding with the existing lattice::err namespace (Error.h's
// fail/fatal/check helpers, which call into this module).
namespace lattice {
namespace err_core {

namespace detail {
// Groups the module's mutable state into one struct so the e2e test harness
// (tests/e2e/harness/NodeContext.cpp) can still snapshot/restore it as a flat
// byte image per simulated node -- the same technique it used against the
// old singleton object.
struct State {
  lattice::hardware::Led* errorLed = nullptr;
  lattice::hardware::SevenSegDisplay* display = nullptr;
  bool initialized = false;
  volatile bool pendingBlink = false;
  lattice::utils::ErrorType pendingBlinkType = lattice::utils::ErrorType::GENERIC;
  bool inCallbackContext = false; // set true when inside ESP-NOW recv task
};
} // namespace detail

void init(lattice::hardware::Led* led, lattice::hardware::SevenSegDisplay* display = nullptr);
void signalError(lattice::core::ErrorTypeDigit t, lattice::core::ModuleDigit m, uint8_t sub,
                 const char* msg = nullptr);
void signalError(lattice::utils::ErrorType type, const char* msg = nullptr);
void setCallbackContext(bool inCallback);
void drainPendingBlink(); // Call from main loop
// Phase I Task 7 (WW): pumps the error LED's non-blocking pulse() state
// machine (Led::update()). signalError()/drainPendingBlink() now only ARM a
// pattern via Led::pulse() — this call is what actually advances it. Call
// from the main loop every iteration, AND from any halt loop (e.g.
// Error.h's fatal()'s while(true){}) that needs the pattern to keep
// animating instead of the LED just sitting at whatever level pulse() left
// it on.
void tick();

#ifdef UNIT_TEST
detail::State& debugStateForTest();
#endif

} // namespace err_core
} // namespace lattice

#endif
