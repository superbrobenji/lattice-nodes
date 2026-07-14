#include "PeerRegistry.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <esp_now.h>
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;

PeerRegistry::PeerRegistry() {
  memset(peerMacs, 0, sizeof(peerMacs));
  memset(deviceMac, 0, sizeof(deviceMac));
}

void PeerRegistry::setDeviceMac(const uint8_t mac[6]) {
  memcpy(deviceMac, mac, 6);
}

PeerInfo* PeerRegistry::find(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, mac, 6) == 0) {
      return &peerMacs[i];
    }
  }
  return nullptr;
}

bool PeerRegistry::append(const PeerInfo& peer) {
  if (peerCount >= MAX_PEERS)
    return false;
  peerMacs[peerCount++] = peer;
  return true;
}

void PeerRegistry::remove(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (lattice::utils::MacAddress(peerMacs[i].mac) == lattice::utils::MacAddress(mac)) {
      peerMacs[i] = peerMacs[--peerCount];
      break;
    }
  }
}

bool PeerRegistry::isPeerInRange(const uint8_t mac[6]) {
  PeerInfo* peer = find(mac);
  if (!peer)
    return false;
  return millis() - peer->lastSeenMillis < lattice::config::STALE_PEER_THRESHOLD_MS;
}

void PeerRegistry::updateLastSeen(const uint8_t mac[6]) {
  if (!mac)
    return;
  if (lattice::utils::MacAddress(mac) == lattice::utils::MacAddress(deviceMac))
    return;
  // Enrollment is the only path for new peers — do not auto-add unknown senders here.
  PeerInfo* p = find(mac);
  if (p) {
    p->lastSeenMillis = millis();
  }
}

// --- EEPROM Peer Management ---
void PeerRegistry::loadFromEEPROM() {
  peerCount = 0;

  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key
  uint8_t peerRecords[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_RECORD_SIZE];
  bool eepromOk = EepromManager::getInstance().loadPeerList(peerRecords, EEPROM_SIZES::MAX_PEERS);

  if (eepromOk) {
    for (int i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
      const uint8_t* record = peerRecords + (i * EEPROM_SIZES::PEER_RECORD_SIZE);
      // Treat all-0xFF MAC as empty slot
      bool valid = false;
      for (int j = 0; j < 6; ++j) {
        if (record[j] != 0xFF) {
          valid = true;
          break;
        }
      }
      if (valid) {
        PeerInfo peer;
        memcpy(peer.mac, record, 6);
        memcpy(peer.publicKey, record + 6, 32);
        peer.lastSeenMillis = 0;
        append(peer);
      }
    }
  }

  // Fallback in dev mode or when list is empty
  if (peerCount == 0) {
    Logger::logln("MESH", "Peer list empty; loading defaults from config", LogLevel::LOG_INFO);
    for (int i = 0; i < lattice::config::NUM_DEFAULT_PEERS; ++i) {
      PeerInfo peer;
      memcpy(peer.mac, lattice::config::DEFAULT_PEERS[i], 6);
      memset(peer.publicKey, 0, 32); // Public key not known yet for config defaults
      peer.lastSeenMillis = 0;
      append(peer);
    }
  }
}

void PeerRegistry::saveToEEPROM() {
  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key
  uint8_t peerRecords[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_RECORD_SIZE];
  memset(peerRecords, 0xFF, sizeof(peerRecords));

  for (size_t i = 0; i < peerCount && i < EEPROM_SIZES::MAX_PEERS; ++i) {
    uint8_t* record = peerRecords + (i * EEPROM_SIZES::PEER_RECORD_SIZE);
    memcpy(record, peerMacs[i].mac, 6);
    memcpy(record + 6, peerMacs[i].publicKey, 32);
  }

  EepromManager::getInstance().savePeerList(peerRecords, peerCount);
}

void PeerRegistry::addAndPersist(const uint8_t mac[6]) {
  if (find(mac) ||
      lattice::utils::MacAddress(mac) == lattice::utils::MacAddress(deviceMac))
    return;

  if (peerCount >= MAX_PEERS) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::MESH, 2,
                       "Peer list full! Cannot add new peer. MAX_PEERS reached.");
    return;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  memset(peer.publicKey, 0, 32); // Public key unknown until enrollment
  peer.lastSeenMillis = millis();
  append(peer);
  saveToEEPROM();
  // Note: ESP-NOW registration (registerPeerWithEspNow) is handled by Mesh layer
  // since it requires devicePrivateKey (a Mesh field) and MeshCrypto (mbedtls).
  Logger::logln("MESH", "Peer added", LogLevel::LOG_DEBUG);
}

void PeerRegistry::removeAndPersist(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (lattice::utils::MacAddress(peerMacs[i].mac) == lattice::utils::MacAddress(mac)) {
      peerMacs[i] = peerMacs[--peerCount]; // swap with last, shrink count
      break;
    }
  }
  saveToEEPROM();
  esp_err_t result = esp_now_del_peer(mac);
  lattice::err::checkEsp(result, lattice::utils::ErrorType::COMMUNICATION_FAIL,
                         "removePeerFromEEPROM: del_peer failed");
  Logger::logln("MESH", "Removed ESP-NOW peer.", LogLevel::LOG_DEBUG);
}

} // namespace mesh
} // namespace lattice
