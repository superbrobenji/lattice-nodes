#ifndef LATTICE_EEPROM_DEVICE_CONFIG_H
#define LATTICE_EEPROM_DEVICE_CONFIG_H
#include <cstdint>
// Needed for lattice::config::TxPowerPreset in loadTxPowerPreset/saveTxPowerPreset's
// signatures below — the one domain header whose public API isn't expressible in
// fundamental types alone.
#include "../../../project_config.h"
namespace lattice {
namespace eeprom {
uint8_t loadAdapterType();
void saveAdapterType(uint8_t adapterType);

lattice::config::TxPowerPreset loadTxPowerPreset();
void saveTxPowerPreset(lattice::config::TxPowerPreset preset);
} // namespace eeprom
} // namespace lattice
#endif
