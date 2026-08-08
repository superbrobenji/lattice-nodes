#include "EepromDiagnostics.h"
#include "EepromCore.h"

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

uint8_t loadRebootCount() {
  if (!core_internal::ensureInitialized())
    return 0;
  return core_internal::nvsGetU8(NVS_KEYS::REBOOT_COUNT, 0);
}

void saveRebootCount(uint8_t count) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_COUNT, count);
  core_internal::persistOrEscalate(NVS_KEYS::REBOOT_COUNT, n, sizeof(uint8_t), false);
}

void saveRebootReason(uint8_t reason) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_REASON, reason);
  core_internal::persistOrEscalate(NVS_KEYS::REBOOT_REASON, n, sizeof(uint8_t), false);
}

uint8_t loadRebootReason() {
  if (!core_internal::ensureInitialized())
    return 0xFF;
  return core_internal::nvsGetU8(NVS_KEYS::REBOOT_REASON, 0xFF);
}

uint32_t loadBootEpoch() {
  if (!core_internal::ensureInitialized())
    return 0;
  if (core_internal::isDevModeInternal())
    return core_internal::devEpochRef();
  return core_internal::nvsGetU32(NVS_KEYS::BOOT_EPOCH, 0);
}

void saveBootEpoch(uint32_t epoch) {
  if (!core_internal::ensureInitialized())
    return;
  if (core_internal::isDevModeInternal()) {
    core_internal::devEpochRef() = epoch;
    return;
  }
  size_t n = core_internal::nvsPutU32(NVS_KEYS::BOOT_EPOCH, epoch);
  core_internal::persistOrEscalate(NVS_KEYS::BOOT_EPOCH, n, sizeof(uint32_t), true);
}

} // namespace eeprom
} // namespace lattice
