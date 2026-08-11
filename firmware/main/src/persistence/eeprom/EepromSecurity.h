#ifndef LATTICE_EEPROM_SECURITY_H
#define LATTICE_EEPROM_SECURITY_H
#include <cstdint>
#include <cstddef>
namespace lattice {
namespace eeprom {
bool loadMeshKey(uint8_t* key, size_t keySize);
void saveMeshKey(const uint8_t* key, size_t keySize);

bool loadKnownMasterMac(uint8_t* mac);
void saveKnownMasterMac(const uint8_t* mac);
void clearKnownMasterMac();

bool loadKnownMasterMacSecondary(uint8_t* mac);
void saveKnownMasterMacSecondary(const uint8_t* mac);
void clearKnownMasterMacSecondary();
} // namespace eeprom
} // namespace lattice
#endif
