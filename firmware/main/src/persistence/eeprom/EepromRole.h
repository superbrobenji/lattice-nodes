#ifndef LATTICE_EEPROM_ROLE_H
#define LATTICE_EEPROM_ROLE_H
namespace lattice {
namespace eeprom {
bool loadMasterFlag();
void saveMasterFlag(bool isMaster);

bool loadDevFlag();
void saveDevFlag(bool isDev);
} // namespace eeprom
} // namespace lattice
#endif
