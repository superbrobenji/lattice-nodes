#ifndef LATTICE_EEPROM_ENROLLMENT_H
#define LATTICE_EEPROM_ENROLLMENT_H
namespace lattice {
namespace eeprom {
bool loadEnrolledFlag();
void saveEnrolledFlag(bool enrolled);
} // namespace eeprom
} // namespace lattice
#endif
