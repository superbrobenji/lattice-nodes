#include "EepromSecurity.h"
#include "EepromCore.h"

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

bool loadMeshKey(uint8_t* key, size_t keySize) {
  if (!core_internal::ensureInitialized())
    return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return false;
  size_t read = core_internal::nvsGetBytes(NVS_KEYS::MESH_KEY, key, keySize);
  return read == keySize;
}

void saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return;
  size_t n = core_internal::nvsPutBytes(NVS_KEYS::MESH_KEY, key, keySize);
  core_internal::persistOrEscalate(NVS_KEYS::MESH_KEY, n, keySize, true);
  core_internal::logOperation("Mesh key saved");
}

bool loadKnownMasterMac(uint8_t* mac) {
  if (!core_internal::ensureInitialized())
    return false;
  size_t read = core_internal::nvsGetBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  if (read != 6)
    return false;
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void saveKnownMasterMac(const uint8_t* mac) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  core_internal::persistOrEscalate(NVS_KEYS::KNOWN_MASTER_MAC, n, 6, true);
  core_internal::logOperation("Known master MAC saved");
}

void clearKnownMasterMac() {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  core_internal::nvsRemove(NVS_KEYS::KNOWN_MASTER_MAC);
  core_internal::logOperation("Known master MAC cleared");
}

bool loadKnownMasterMacSecondary(uint8_t* mac) {
  if (!core_internal::ensureInitialized())
    return false;
  size_t read = core_internal::nvsGetBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  if (read != 6)
    return false;
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void saveKnownMasterMacSecondary(const uint8_t* mac) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  core_internal::persistOrEscalate(NVS_KEYS::KNOWN_MASTER_MAC_SEC, n, 6, true);
  core_internal::logOperation("Known secondary master MAC saved");
}

void clearKnownMasterMacSecondary() {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  core_internal::nvsRemove(NVS_KEYS::KNOWN_MASTER_MAC_SEC);
}

} // namespace eeprom
} // namespace lattice
