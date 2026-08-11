#include "EepromIdentity.h"
#include "EepromCore.h"
#include <cstring>

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!core_internal::ensureInitialized())
    return false;
  size_t privRead = core_internal::nvsGetBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubRead = core_internal::nvsGetBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  if (privRead != 32 || pubRead != 32)
    return false;
  uint32_t stored = core_internal::nvsGetU32(NVS_KEYS::KEYPAIR_CRC, 0xFFFFFFFF);
  if (stored == 0xFFFFFFFF)
    return false;
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = core_internal::crc16(both, 64);
  if (static_cast<uint16_t>(stored) != computed) {
    LATTICE_LOGLN("NVS", "Keypair CRC mismatch", lattice::utils::LogLevel::LOG_WARN);
    return false;
  }
  return true;
}

void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t nPriv = core_internal::nvsPutBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  core_internal::persistOrEscalate(NVS_KEYS::PRIVATE_KEY, nPriv, 32, true);
  size_t nPub = core_internal::nvsPutBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  core_internal::persistOrEscalate(NVS_KEYS::PUBLIC_KEY, nPub, 32, true);
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = core_internal::crc16(both, 64);
  size_t nCrc = core_internal::nvsPutU32(NVS_KEYS::KEYPAIR_CRC, static_cast<uint32_t>(crc));
  core_internal::persistOrEscalate(NVS_KEYS::KEYPAIR_CRC, nCrc, sizeof(uint32_t), true);
  core_internal::logOperation("Keypair saved");
}

uint8_t loadNodeId() {
  if (!core_internal::ensureInitialized())
    return 0;
  return core_internal::nvsGetU8(NVS_KEYS::NODE_ID, 0);
}

void saveNodeId(uint8_t nodeId) {
  if (!core_internal::ensureInitialized())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::NODE_ID, nodeId);
  core_internal::persistOrEscalate(NVS_KEYS::NODE_ID, n, sizeof(uint8_t), false);
  core_internal::logOperation("saveNodeId");
}

} // namespace eeprom
} // namespace lattice
