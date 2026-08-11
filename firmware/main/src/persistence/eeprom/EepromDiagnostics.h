#ifndef LATTICE_EEPROM_DIAGNOSTICS_H
#define LATTICE_EEPROM_DIAGNOSTICS_H
#include <cstdint>
namespace lattice {
namespace eeprom {
uint8_t loadRebootCount();
void saveRebootCount(uint8_t count);
void saveRebootReason(uint8_t reason);
uint8_t loadRebootReason();

uint32_t loadBootEpoch();
void saveBootEpoch(uint32_t epoch);
} // namespace eeprom
} // namespace lattice
#endif
