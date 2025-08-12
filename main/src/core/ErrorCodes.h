#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include <Arduino.h>

namespace planetopia {
namespace core {

// First digit – error type / severity
enum class ErrorTypeDigit : uint8_t {
  OK        = 0,
  HARDWARE  = 1,
  COMM      = 2,
  MEMORY    = 3,
  CONFIG    = 4,
  LOGIC     = 5,
};

// Second digit – high-level module
enum class ModuleDigit : uint8_t {
  CORE        = 0,
  ADAPTER     = 1,
  MESH        = 2,
  PERSISTENCE = 3,
  NETWORK     = 4,
};

// Helper to build 4-digit error codes
constexpr uint16_t makeErrorCode(ErrorTypeDigit t, ModuleDigit m, uint8_t sub)
{
    return static_cast<uint16_t>(t) * 1000 +
           static_cast<uint16_t>(m) * 100  +
           static_cast<uint16_t>(sub % 100);
}

} // namespace core
} // namespace planetopia

#endif /* ERROR_CODES_H */
