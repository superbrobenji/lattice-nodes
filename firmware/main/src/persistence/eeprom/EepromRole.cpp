#include "EepromRole.h"
#include "EepromCore.h"

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

bool loadMasterFlag() {
  if (!core_internal::ensureInitialized())
    return false;
  return core_internal::nvsGetBool(NVS_KEYS::MASTER_FLAG, false);
}

void saveMasterFlag(bool isMaster) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutBool(NVS_KEYS::MASTER_FLAG, isMaster);
  core_internal::persistOrEscalate(NVS_KEYS::MASTER_FLAG, n, 1, false);
  core_internal::logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

bool loadDevFlag() {
  if (!core_internal::ensureInitialized())
    return false;
  return core_internal::nvsGetBool(NVS_KEYS::DEV_FLAG, false);
}

void saveDevFlag(bool isDev) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutBool(NVS_KEYS::DEV_FLAG, isDev);
  core_internal::persistOrEscalate(NVS_KEYS::DEV_FLAG, n, 1, false);
}

} // namespace eeprom
} // namespace lattice
