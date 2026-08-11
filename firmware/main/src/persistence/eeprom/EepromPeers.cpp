#include "EepromPeers.h"
#include "EepromCore.h"
#include <cstring>
#include <cstdio>

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

// Phase I Task 6 (JJ): record[0..PEER_RECORD_SIZE) <-> the "peerN" NVS key.
// Returns false (record left untouched by the caller's own prefill, if any)
// when the key is absent — the same "not yet saved" signal
// loadPeerList()'s old single-blob short-read path used to report per-record
// instead of for the whole list at once.
bool loadPeerRecord(uint8_t index, uint8_t* record) {
  if (!core_internal::ensureInitialized())
    return false;
  if (index >= EEPROM_SIZES::MAX_PEERS)
    return false;
  char key[8];
  core_internal::peerKey(index, key, sizeof(key));
  size_t read = core_internal::nvsGetBytes(key, record, EEPROM_SIZES::PEER_RECORD_SIZE);
  return read == EEPROM_SIZES::PEER_RECORD_SIZE;
}

void savePeerRecord(uint8_t index, const uint8_t* record) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  if (index >= EEPROM_SIZES::MAX_PEERS)
    return;
  char key[8];
  core_internal::peerKey(index, key, sizeof(key));
  size_t n = core_internal::nvsPutBytes(key, record, EEPROM_SIZES::PEER_RECORD_SIZE);
  // securityRelevant=true: each record carries a peer's E2E public key —
  // trust material, same tier as MESH_KEY/KNOWN_MASTER_MAC below.
  core_internal::persistOrEscalate(key, n, EEPROM_SIZES::PEER_RECORD_SIZE, true);
}

void erasePeerRecord(uint8_t index) {
  if (!core_internal::ensureInitialized())
    return;
  if (index >= EEPROM_SIZES::MAX_PEERS)
    return;
  char key[8];
  core_internal::peerKey(index, key, sizeof(key));
  core_internal::nvsRemove(key); // no-op (returns false, ignored) if the key was never set
}

// Thin loop over loadPeerRecord — kept for main.cpp's default-peer bootstrap
// and existing host tests. peerRecords must hold maxPeers *
// EEPROM_SIZES::PEER_RECORD_SIZE bytes.
bool loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  if (!core_internal::ensureInitialized())
    return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS)
    return false;
  size_t maxBytes = maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  // Pre-fill with the "empty slot" sentinel (0xFF) — a slot whose key was
  // never saved is left at this sentinel rather than read from
  // uninitialized/stale caller memory, same guarantee the old single-blob
  // prefill provided.
  memset(peerRecords, 0xFF, maxBytes);
  bool anyFound = false;
  for (size_t i = 0; i < maxPeers; ++i) {
    uint8_t* record = peerRecords + i * EEPROM_SIZES::PEER_RECORD_SIZE;
    if (loadPeerRecord(static_cast<uint8_t>(i), record)) {
      anyFound = true;
    } else {
      memset(record, 0xFF, EEPROM_SIZES::PEER_RECORD_SIZE);
    }
  }
  return anyFound;
}

// Thin loop over savePeerRecord/erasePeerRecord — kept for main.cpp's
// default-peer bootstrap and existing host tests.
void savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS)
    return;
  for (uint8_t i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
    if (i < numPeers) {
      savePeerRecord(i, peerRecords + static_cast<size_t>(i) * EEPROM_SIZES::PEER_RECORD_SIZE);
    } else {
      erasePeerRecord(i);
    }
  }
  // Phase I Task 7 (TT): String() temporary eliminated.
  char numPeersBuf[12];
  snprintf(numPeersBuf, sizeof(numPeersBuf), "%zu", numPeers);
  core_internal::logOperation("Peer list saved", numPeersBuf);
}

bool hasPeers() {
  if (!core_internal::ensureInitialized())
    return false;
  for (uint8_t i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
    char key[8];
    core_internal::peerKey(i, key, sizeof(key));
    if (core_internal::nvsHasKey(key))
      return true;
  }
  return false;
}

void clearPeerList() {
  if (!core_internal::ensureInitialized())
    return;
  for (uint8_t i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
    erasePeerRecord(i);
  }
  core_internal::logOperation("Peer list cleared");
}

} // namespace eeprom
} // namespace lattice
