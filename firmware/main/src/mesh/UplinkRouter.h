#pragma once
#include <cstdint>
#include "NeighborTable.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

// Uplink mirror of DownlinkRouter's forwarding-peer bookkeeping (round 2 task 10) —
// here a single-slot "LRU" since a node only ever forwards uplink to one next hop
// at a time. Owns findNextHopToMaster, moved verbatim from Mesh.
class UplinkRouter {
public:
  // Returns a pointer to a scratch PeerInfo (not a `peers` member — mirrors the
  // original nextHopScratch pattern) representing the chosen next hop, or nullptr
  // if no route exists. Side effect: registers the chosen hop with
  // MeshTransport::registerPeerWithEspNow and evicts the prior forwardingPeer from
  // ESP-NOW if it changed and isn't itself an enrolled peer or the current master.
  PeerInfo* findNextHopToMaster(const MasterInfo& currentMaster, PeerRegistry& peers,
                                NeighborTable& neighbors, const uint8_t* deviceMac, uint64_t nowMs);

private:
  uint8_t forwardingPeer[6]{};
  PeerInfo nextHopScratch{};
};

} // namespace mesh
} // namespace lattice
