#pragma once
#include <cstdint>
#include <cstring>
#include "src/persistence/EepromManager.h"
#include "src/network/MacAddress.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

using lattice::utils::EEPROM_SIZES::MAX_PEERS;

// Peer info struct for RAM and EEPROM storage
struct PeerInfo {
  uint8_t mac[6];
  uint8_t publicKey[32]; // Curve25519 public key (zero = not yet known)
  // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
  // millis() -> esp_timer_get_time()/1000ULL swap (esp_timer's epoch is
  // microseconds-since-boot as an int64_t; a 32-bit ms field would wrap
  // ~49 days after boot).
  uint64_t lastSeenMs;
};

// Master routing info
struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance; // Hops to master
};

class PeerRegistry {
public:
  PeerRegistry();
  void setDeviceMac(const uint8_t* mac);

  PeerInfo* find(const uint8_t* mac);
  const PeerInfo* find(const uint8_t* mac) const;
  bool append(const PeerInfo& peer);
  void remove(const uint8_t* mac);
  bool isPeerInRange(const uint8_t* mac) const;
  void updateLastSeen(const uint8_t* mac);

  void loadFromEEPROM();
  void saveToEEPROM();
  void addAndPersist(const uint8_t* mac);
  void removeAndPersist(const uint8_t* mac);

  size_t count() const { return peerCount; }
  const PeerInfo& at(size_t i) const { return peerMacs[i]; }
  PeerInfo* begin() { return peerMacs; }
  PeerInfo* end() { return peerMacs + peerCount; }
  const PeerInfo* begin() const { return peerMacs; }
  const PeerInfo* end() const { return peerMacs + peerCount; }

private:
  PeerInfo peerMacs[MAX_PEERS]{};
  size_t peerCount{0};
  uint8_t deviceMac[6]{};
};

} // namespace mesh
} // namespace lattice
