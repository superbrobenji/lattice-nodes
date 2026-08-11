#pragma once
#include <cstdint>
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

// Bridges MasterInfo + PeerRegistry + Enrollment + E2EKeyStore — free functions
// rather than a method on any one of the 4 collaborators (round 2 task 9), matching
// this codebase's existing convention for cross-cutting utilities (network/mac_table.h,
// network/MacEq.h, network/mem.h, network/hw_mac.h). Adding these as methods on
// E2EKeyStore would give it an artificial dependency on PeerRegistry/Enrollment it
// otherwise deliberately doesn't have.

// Returns k_up/k_down for the current master (leaf side); false if not enrolled
// or master pubkey unknown.
inline bool masterE2EKeys(const MasterInfo& currentMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, const uint8_t** kUp,
                          const uint8_t** kDown) {
  if (!enrollment.hasKnownMaster())
    return false;
  PeerInfo* master = peers.find(currentMaster.mac);
  if (!master)
    return false;
  return e2eKeys.getKeys(master->mac, enrollment.getPrivateKey(), master->publicKey, kUp, kDown);
}

// Returns keys for an enrolled origin peer (master side); false if unknown peer.
inline bool peerE2EKeys(const uint8_t* originMac, PeerRegistry& peers, Enrollment& enrollment,
                        E2EKeyStore& e2eKeys, const uint8_t** kUp, const uint8_t** kDown) {
  PeerInfo* peer = peers.find(originMac);
  if (!peer)
    return false;
  return e2eKeys.getKeys(peer->mac, enrollment.getPrivateKey(), peer->publicKey, kUp, kDown);
}

} // namespace mesh
} // namespace lattice
