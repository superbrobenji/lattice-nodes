#include "EepromEnrollment.h"
#include "EepromCore.h"

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

bool loadEnrolledFlag() {
  if (!core_internal::ensureInitialized())
    return false;
  return core_internal::nvsGetBool(NVS_KEYS::ENROLLED_FLAG, false);
}

void saveEnrolledFlag(bool enrolled) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t n = core_internal::nvsPutBool(NVS_KEYS::ENROLLED_FLAG, enrolled);
  core_internal::persistOrEscalate(NVS_KEYS::ENROLLED_FLAG, n, 1, false);
}

} // namespace eeprom
} // namespace lattice
