#include "UplinkRouter.h"
#include <cstring>
#include <esp_now.h>
#include "MeshTransport.h"
#include "src/network/MacEq.h"

namespace lattice {
namespace mesh {

// Moved verbatim from Mesh::findNextHopToMaster (Mesh.cpp:61-105 pre-Task-10)
// — only change: currentMaster/peers/neighbors/deviceMac are now explicit
// parameters instead of implicit Mesh member access; nowMs is passed in
// rather than calling esp_timer_get_time()/1000ULL here directly (the one
// caller-local exception being NeighborTable::selectNextHop's own use of
// nowMs, which stays inline since it's a free ESP-IDF call not a
// collaborator), and lattice::mesh::crypto::registerPeerWithEspNow becomes
// MeshTransport::registerPeerWithEspNow (static, per Round 1 Task 4).
PeerInfo* UplinkRouter::findNextHopToMaster(const MasterInfo& currentMaster, PeerRegistry& peers,
                                            NeighborTable& neighbors, const uint8_t* deviceMac,
                                            uint64_t nowMs) {
  if (currentMaster.distance == 0xFF)
    return nullptr;

  // Prefer an enrolled peer that is the direct master and in range (distance 1,
  // the common single-hop case) — keeps the existing behavior and E2E peering.
  PeerInfo* direct = peers.find(currentMaster.mac);
  if (direct && currentMaster.distance == 1 && peers.isPeerInRange(direct->mac) &&
      !lattice::mac::eq(direct->mac, deviceMac))
    return direct;

  // Multi-hop (spec §3): pick the freshest neighbor strictly closer to the
  // master from the NeighborTable. The relay need not be an enrolled peer.
  uint8_t hopMac[6];
  if (!neighbors.selectNextHop(currentMaster.distance, nowMs, hopMac))
    return nullptr;
  if (lattice::mac::eq(hopMac, deviceMac))
    return nullptr;

  // Bound the auto-registered forwarding peer to exactly one (spec §2:
  // "20-peer cap, LRU-evicted"). A node forwards uplink to only one next hop
  // at a time, so if we previously auto-registered a DIFFERENT relay, evict
  // it before registering the new one — otherwise a beacon-flooding attacker
  // spoofing distinct relay MACs could exhaust the ~20-slot ESP-NOW peer
  // table (no self-heal, no reboot) and blackhole the real uplink. Never
  // evict an enrolled peer (master or sensor) — those live in `peers` and
  // are managed exclusively by the enrollment path.
  static const uint8_t kZeroMac[6] = {0, 0, 0, 0, 0, 0};
  bool isNewRelay = !lattice::mac::eq(forwardingPeer, hopMac);
  bool forwardingPeerSet = !lattice::mac::eq(forwardingPeer, kZeroMac);
  if (forwardingPeerSet && isNewRelay && !peers.find(forwardingPeer) &&
      !lattice::mac::eq(forwardingPeer, currentMaster.mac)) {
    if (esp_now_is_peer_exist(forwardingPeer))
      esp_now_del_peer(forwardingPeer);
  }

  // Auto-register the chosen next hop as an unencrypted ESP-NOW peer (spec §3).
  // Idempotent — registerPeerWithEspNow no-ops if the peer already exists.
  MeshTransport::registerPeerWithEspNow(hopMac);
  memcpy(forwardingPeer, hopMac, 6);

  memcpy(nextHopScratch.mac, hopMac, 6);
  return &nextHopScratch;
}

} // namespace mesh
} // namespace lattice
