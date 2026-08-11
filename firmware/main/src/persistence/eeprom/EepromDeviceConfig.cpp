#include "EepromDeviceConfig.h"
#include "EepromCore.h"

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

uint8_t loadAdapterType() {
  if (!core_internal::ensureInitialized())
    return 0;
  return core_internal::nvsGetU8(NVS_KEYS::ADAPTER_TYPE, 0);
}

void saveAdapterType(uint8_t adapterType) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::ADAPTER_TYPE, adapterType);
  core_internal::persistOrEscalate(NVS_KEYS::ADAPTER_TYPE, n, sizeof(uint8_t), false);
}

lattice::config::TxPowerPreset loadTxPowerPreset() {
  if (!core_internal::ensureInitialized())
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  uint8_t val = core_internal::nvsGetU8(NVS_KEYS::TX_POWER_PRESET, 0xFF);
  if (val > 2)
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  return static_cast<lattice::config::TxPowerPreset>(val);
}

void saveTxPowerPreset(lattice::config::TxPowerPreset preset) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::TX_POWER_PRESET, static_cast<uint8_t>(preset));
  core_internal::persistOrEscalate(NVS_KEYS::TX_POWER_PRESET, n, sizeof(uint8_t), false);
  core_internal::logOperation("TX power preset saved");
}

} // namespace eeprom
} // namespace lattice
