#ifndef PLANETOPA_ERRORCODES_H
#define PLANETOPA_ERRORCODES_H
namespace planetopia {
namespace core {
enum class ErrorTypeDigit : uint8_t {
  GENERIC = 1,
  SENSOR = 2,
  COMM = 3,
  MEMORY = 4,
  HARDWARE = 5,
  CONFIG = 6
};
enum class ModuleDigit : uint8_t { CORE = 1, ADAPTER = 2, MESH = 3, EEPROM = 4, HW = 5 };
constexpr uint16_t makeErrorCode(ErrorTypeDigit t, ModuleDigit m, uint8_t sub) {
  // Compose a 3-digit decimal code TMS, where:
  // T = ErrorTypeDigit (1-6)
  // M = ModuleDigit (1-5)
  // S = sub-code (0-9)
  // This representation is easier to read on the 7-segment display than the previous
  // bit-packed hexadecimal value.
  return static_cast<uint16_t>(static_cast<uint16_t>(t) * 100 + static_cast<uint16_t>(m) * 10 +
                               (sub % 10));
}
} // namespace core
} // namespace planetopia
#endif
