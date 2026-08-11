#include "PeerRegistry.h"
#include "src/persistence/eeprom/EepromPeers.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/network/MacEq.h"
#include "src/network/mac_table.h"
#include <esp_now.h>
#include <esp_timer.h>
#include <cstddef>
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;

PeerRegistry::PeerRegistry() {
  memset(peerMacs, 0, sizeof(peerMacs));
  memset(deviceMac, 0, sizeof(deviceMac));
}

void PeerRegistry::setDeviceMac(const uint8_t* mac) {
  memcpy(deviceMac, mac, 6);
}

// Thinned via lattice::mac_table::find (Phase H2 audit item Y). peerMacs has
// no per-entry "valid" bit — every slot in [0, peerCount) is live — so the
// found index maps straight to a pointer, no extra flag check needed.
PeerInfo* PeerRegistry::find(const uint8_t* mac) {
  size_t idx =
      lattice::mac_table::find(peerMacs, peerCount, sizeof(PeerInfo), offsetof(PeerInfo, mac), mac);
  return idx == SIZE_MAX ? nullptr : &peerMacs[idx];
}

const PeerInfo* PeerRegistry::find(const uint8_t* mac) const {
  size_t idx =
      lattice::mac_table::find(peerMacs, peerCount, sizeof(PeerInfo), offsetof(PeerInfo, mac), mac);
  return idx == SIZE_MAX ? nullptr : &peerMacs[idx];
}

bool PeerRegistry::append(const PeerInfo& peer) {
  if (peerCount >= MAX_PEERS)
    return false;
  peerMacs[peerCount++] = peer;
  return true;
}

void PeerRegistry::remove(const uint8_t* mac) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (lattice::mac::eq(peerMacs[i].mac, mac)) {
      peerMacs[i] = peerMacs[--peerCount];
      break;
    }
  }
}

bool PeerRegistry::isPeerInRange(const uint8_t* mac) const {
  const PeerInfo* peer = find(mac);
  if (!peer)
    return false;
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - peer->lastSeenMs <
         lattice::config::STALE_PEER_THRESHOLD_MS;
}

void PeerRegistry::updateLastSeen(const uint8_t* mac) {
  if (!mac)
    return;
  if (lattice::mac::eq(mac, deviceMac))
    return;
  // Enrollment is the only path for new peers — do not auto-add unknown senders here.
  PeerInfo* p = find(mac);
  if (p) {
    p->lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  }
}

// --- EEPROM Peer Management ---
//
// Phase I Task 6 (JJ): per-key blob load/save (EepromManager::loadPeerRecord/
// savePeerRecord/erasePeerRecord — "peer0".."peer9" NVS keys) replaces the old
// single 380-byte ("peers" key) blob. real nvs_get_blob has no offset/partial-
// read mode (verified against IDF v5.5.1 nvs.h), so per-key naming is the only
// way to avoid materializing the whole MAX_PEERS*PEER_RECORD_SIZE array on the
// stack here — each iteration now only needs one PEER_RECORD_SIZE (38-byte)
// scratch buffer instead of a 380-byte one. Changes the on-flash NVS layout
// ("peers" -> "peer0".."peer9") — requires a device reflash on Phase I close
// (accepted per project posture; see task-6-report.md).
void PeerRegistry::loadFromEEPROM() {
  peerCount = 0;

  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key.
  for (uint8_t i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
    uint8_t record[EEPROM_SIZES::PEER_RECORD_SIZE];
    if (!lattice::eeprom::loadPeerRecord(i, record))
      continue; // key not present (never saved / already erased) — skip slot

    // Treat all-0xFF MAC as empty slot (defensive — loadPeerRecord already
    // reports absent keys via its bool return, but this guards a record that
    // was somehow persisted as the old sentinel pattern).
    bool valid = false;
    for (int j = 0; j < 6; ++j) {
      if (record[j] != 0xFF) {
        valid = true;
        break;
      }
    }
    if (!valid)
      continue;

    PeerInfo peer;
    memcpy(peer.mac, record, 6);
    memcpy(peer.publicKey, record + 6, 32);
    peer.lastSeenMs = 0;
    append(peer);
  }

  // Fallback in dev mode or when list is empty
  if (peerCount == 0) {
    LATTICE_LOGLN("MESH", "Peer list empty; loading defaults from config", LogLevel::LOG_INFO);
    for (int i = 0; i < lattice::config::NUM_DEFAULT_PEERS; ++i) {
      PeerInfo peer;
      memcpy(peer.mac, lattice::config::DEFAULT_PEERS[i], 6);
      memset(peer.publicKey, 0, 32); // Public key not known yet for config defaults
      peer.lastSeenMs = 0;
      append(peer);
    }
  }
}

void PeerRegistry::saveToEEPROM() {
  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key. Slots
  // [0, peerCount) are written; slots [peerCount, MAX_PEERS) are erased so a
  // shrink (peer removal) doesn't leave stale data behind in a higher-index
  // "peerN" key — unlike the old single-blob save, each key here persists
  // independently, so a former tail entry must be explicitly cleared rather
  // than implicitly dropped by writing a shorter blob.
  uint8_t record[EEPROM_SIZES::PEER_RECORD_SIZE];
  for (uint8_t i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
    if (i < peerCount) {
      memcpy(record, peerMacs[i].mac, 6);
      memcpy(record + 6, peerMacs[i].publicKey, 32);
      lattice::eeprom::savePeerRecord(i, record);
    } else {
      lattice::eeprom::erasePeerRecord(i);
    }
  }
}

void PeerRegistry::addAndPersist(const uint8_t* mac) {
  if (find(mac) || lattice::mac::eq(mac, deviceMac))
    return;

  if (peerCount >= MAX_PEERS) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::MESH, 2,
                       "Peer list full! Cannot add new peer. MAX_PEERS reached.");
    return;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  memset(peer.publicKey, 0, 32); // Public key unknown until enrollment
  peer.lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  append(peer);
  saveToEEPROM();
  // Note: ESP-NOW registration (registerPeerWithEspNow) is handled by Mesh layer
  // since it requires devicePrivateKey (a Mesh field) and MeshCrypto (mbedtls).
  LATTICE_LOGLN("MESH", "Peer added", LogLevel::LOG_DEBUG);
}

void PeerRegistry::removeAndPersist(const uint8_t* mac) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (lattice::mac::eq(peerMacs[i].mac, mac)) {
      peerMacs[i] = peerMacs[--peerCount]; // swap with last, shrink count
      break;
    }
  }
  saveToEEPROM();
  esp_err_t result = esp_now_del_peer(mac);
  lattice::err::checkEsp(result, lattice::utils::ErrorType::COMMUNICATION_FAIL,
                         "removePeerFromEEPROM: del_peer failed");
  LATTICE_LOGLN("MESH", "Removed ESP-NOW peer.", LogLevel::LOG_DEBUG);
}

} // namespace mesh
} // namespace lattice
