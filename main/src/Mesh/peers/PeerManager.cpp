#include "PeerManager.h"
#include "src/Mesh/Mesh.h"
#include "src/network/MacAddress.h"
#include "../../../project_config.h"  // for STALE_PEER_THRESHOLD_MS
using planetopia::mesh::PeerInfo;
#include <cstring>
#include <algorithm>

namespace planetopia {
namespace utils {

PeerManager::PeerManager()
  : staleThresholdMs_(planetopia::config::STALE_PEER_THRESHOLD_MS), maxPeers_(EEPROM_SIZES::MAX_PEERS), lastFlushMs_(millis()) {
  peers_.reserve(maxPeers_);  // static allocation at startup
}

bool PeerManager::addPeer(const uint8_t mac[6], bool saveToEEPROM) {
  planetopia::err::check(mac != nullptr, planetopia::utils::ErrorType::CONFIG_ERROR, "addPeer: mac is null");
  if (!isValidMac(mac)) {
    Logger::logln("PEER", "Invalid MAC address provided", LogLevel::LOG_ERROR);
    return false;
  }

  if (isPeer(mac)) {
    Logger::logln("PEER", "Peer already exists", LogLevel::LOG_DEBUG);
    return true;  // Already exists
  }

  if (isPeerListFull()) {
    Logger::logln("PEER", "Peer list is full", LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                         planetopia::core::ModuleDigit::MESH,
                         1,
                         "Peer list full! Cannot add new peer.");
    return false;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  peer.lastSeenMillis = millis();
  peers_.push_back(peer);

  if (saveToEEPROM) {
    if (millis() - lastFlushMs_ > FLUSH_INTERVAL_MS) {
      savePeersToEEPROM();
      lastFlushMs_ = millis();
    }
  }

  logPeerOperation("Added peer", mac, true);
  return true;
}

bool PeerManager::removePeer(const uint8_t mac[6], bool saveToEEPROM) {
  planetopia::err::check(mac != nullptr, planetopia::utils::ErrorType::CONFIG_ERROR, "removePeer: mac is null");
  auto it = std::find_if(peers_.begin(), peers_.end(),
                         [mac](const PeerInfo& peer) {
                           return memcmp(peer.mac, mac, 6) == 0;
                         });

  if (it != peers_.end()) {
    peers_.erase(it);

    if (saveToEEPROM) {
      if (millis() - lastFlushMs_ > FLUSH_INTERVAL_MS) {
        savePeersToEEPROM();
        lastFlushMs_ = millis();
      }
    }

    logPeerOperation("Removed peer", mac, true);
    return true;
  }

  logPeerOperation("Remove peer failed", mac, false);
  return false;
}

bool PeerManager::isPeer(const uint8_t mac[6]) const {
  return findPeer(mac) != nullptr;
}

PeerManager::PeerStatus PeerManager::getPeerStatus(const uint8_t mac[6]) const {
  const PeerInfo* peer = findPeer(mac);
  if (!peer) {
    return PeerStatus::UNKNOWN;
  }

  unsigned long timeSinceLastSeen = millis() - peer->lastSeenMillis;

  if (timeSinceLastSeen < staleThresholdMs_) {
    return PeerStatus::ONLINE;
  } else if (timeSinceLastSeen < staleThresholdMs_ * 2) {
    return PeerStatus::STALE;
  } else {
    return PeerStatus::OFFLINE;
  }
}

PeerManager::DiscoveryResult PeerManager::discoverPeer(const uint8_t mac[6]) {
  DiscoveryResult result = {};
  memcpy(result.mac, mac, 6);

  // This is a simplified discovery - in a real implementation,
  // you might send a ping and wait for response
  if (isPeer(mac)) {
    result.success = true;
    result.signalStrength = 100;  // Placeholder
    Logger::logln("PEER", "Peer discovered (already known)", LogLevel::LOG_DEBUG);
  } else {
    result.success = false;
    result.errorMessage = "Peer not found";
    Logger::logln("PEER", "Peer discovery failed", LogLevel::LOG_DEBUG);
  }

  return result;
}

void PeerManager::updatePeerLastSeen(const uint8_t mac[6]) {
  PeerInfo* peer = findPeer(mac);
  if (peer) {
    peer->lastSeenMillis = millis();
    Logger::logln("PEER", "Updated peer last seen", LogLevel::LOG_DEBUG);
  }
}

void PeerManager::cleanupStalePeers(uint32_t staleThresholdMs) {
  uint32_t currentTime = millis();
  auto it = peers_.begin();

  while (it != peers_.end()) {
    if (currentTime - it->lastSeenMillis > staleThresholdMs) {
      Logger::logln("PEER", "Removing stale peer", LogLevel::LOG_INFO);
      it = peers_.erase(it);
    } else {
      ++it;
    }
  }

  if (it != peers_.end()) {
    savePeersToEEPROM();
    lastFlushMs_ = millis();
  }
}

bool PeerManager::loadPeersFromEEPROM() {
  peers_.clear();

  uint8_t peerList[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_MAC_SIZE];
  if (EEPROM_Manager::getInstance().loadPeerList(peerList, EEPROM_SIZES::MAX_PEERS)) {
    for (int i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
      uint8_t mac[6];
      bool allFF = true, all00 = true;

      for (int j = 0; j < 6; ++j) {
        mac[j] = peerList[i * 6 + j];
        if (mac[j] != 0xFF) allFF = false;
        if (mac[j] != 0x00) all00 = false;
      }

      if (!allFF && !all00) {
        PeerInfo peer;
        memcpy(peer.mac, mac, 6);
        peer.lastSeenMillis = 0;  // Will update at runtime
        peers_.push_back(peer);
      }
    }

    Logger::logln("PEER", "Loaded " + String(peers_.size()) + " peers from EEPROM", LogLevel::LOG_INFO);
    return true;
  }

  Logger::logln("PEER", "Failed to load peers from EEPROM", LogLevel::LOG_ERROR);
  return false;
}

bool PeerManager::savePeersToEEPROM() {
  uint8_t peerList[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_MAC_SIZE];
  memset(peerList, 0xFF, sizeof(peerList));  // Initialize with 0xFF

  for (size_t i = 0; i < peers_.size() && i < EEPROM_SIZES::MAX_PEERS; ++i) {
    for (int j = 0; j < 6; ++j) {
      peerList[i * 6 + j] = peers_[i].mac[j];
    }
  }

  EEPROM_Manager::getInstance().savePeerList(peerList, peers_.size());
  Logger::logln("PEER", "Saved " + String(peers_.size()) + " peers to EEPROM", LogLevel::LOG_DEBUG);
  return true;
}

void PeerManager::clearAllPeers() {
  peers_.clear();
  savePeersToEEPROM();
  Logger::logln("PEER", "Cleared all peers", LogLevel::LOG_INFO);
}

bool PeerManager::addPeerToESPNow(const uint8_t mac[6], const uint8_t* encryptionKey) {
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = true;
  memcpy(info.lmk, encryptionKey, 16);
  planetopia::err::checkEsp(esp_now_add_peer(&info), planetopia::utils::ErrorType::COMMUNICATION_FAIL, "addPeerToESPNow failed");
  Logger::logln("PEER", "Added ESP-NOW peer successfully", LogLevel::LOG_DEBUG);
  return true;
}

bool PeerManager::removePeerFromESPNow(const uint8_t mac[6]) {
  esp_err_t result = esp_now_del_peer(mac);
  planetopia::err::checkEsp(result, planetopia::utils::ErrorType::COMMUNICATION_FAIL, "removePeerFromESPNow failed");
  Logger::logln("PEER", "Removed ESP-NOW peer successfully", LogLevel::LOG_DEBUG);
  return true;
}

PeerInfo* PeerManager::findPeer(const uint8_t mac[6]) {
  auto it = std::find_if(peers_.begin(), peers_.end(),
                         [mac](const PeerInfo& peer) {
                           return memcmp(peer.mac, mac, 6) == 0;
                         });

  return (it != peers_.end()) ? &(*it) : nullptr;
}

const PeerInfo* PeerManager::findPeer(const uint8_t mac[6]) const {
  auto it = std::find_if(peers_.begin(), peers_.end(),
                         [mac](const PeerInfo& peer) {
                           return memcmp(peer.mac, mac, 6) == 0;
                         });

  return (it != peers_.end()) ? &(*it) : nullptr;
}

bool PeerManager::isValidMac(const uint8_t mac[6]) const {
  // Check if MAC is not all zeros or all 0xFF
  bool allZero = true;
  bool allFF = true;

  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0x00) allZero = false;
    if (mac[i] != 0xFF) allFF = false;
  }

  return !allZero && !allFF;
}

void PeerManager::logPeerOperation(const char* operation, const uint8_t mac[6], bool success) {
  String macStr = planetopia::utils::MacAddress(mac).toString();

  if (success) {
    Logger::logln("PEER", String(operation) + " - " + macStr, LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("PEER", String(operation) + " failed - " + macStr, LogLevel::LOG_ERROR);
  }
}

void PeerManager::periodicFlush() {
  if (millis() - lastFlushMs_ > FLUSH_INTERVAL_MS) {
    savePeersToEEPROM();
    lastFlushMs_ = millis();
  }
}

}  // namespace utils
}  // namespace planetopia
