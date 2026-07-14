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
  uint32_t lastSeenMillis;
};

// Master routing info
struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance;   // Hops to master
  uint8_t nextHop[6]; // Next hop MAC
};

class PeerRegistry {
public:
  PeerInfo peerMacs[MAX_PEERS]{};
  size_t peerCount{0};

  PeerRegistry();
  void setDeviceMac(const uint8_t mac[6]);

  PeerInfo* find(const uint8_t mac[6]);
  const PeerInfo* find(const uint8_t mac[6]) const;
  bool append(const PeerInfo& peer);
  void remove(const uint8_t mac[6]);
  bool isPeerInRange(const uint8_t mac[6]) const;
  void updateLastSeen(const uint8_t mac[6]);

  void loadFromEEPROM();
  void saveToEEPROM();
  void addAndPersist(const uint8_t mac[6]);
  void removeAndPersist(const uint8_t mac[6]);

  size_t count() const { return peerCount; }

private:
  uint8_t deviceMac[6]{};
};

} // namespace mesh
} // namespace lattice
