#ifndef LATTICE_EEPROM_IDENTITY_H
#define LATTICE_EEPROM_IDENTITY_H
#include <cstdint>
namespace lattice {
namespace eeprom {
bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32);
void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32);
uint8_t loadNodeId();
void saveNodeId(uint8_t nodeId);
} // namespace eeprom
} // namespace lattice
#endif
