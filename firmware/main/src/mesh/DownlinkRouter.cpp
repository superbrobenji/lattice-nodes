#include "DownlinkRouter.h"
#include "src/network/MacEq.h"
#include <cstring>
#include <esp_now.h>

namespace lattice {
namespace mesh {

// Translates processAdapterData's routing block (the original
// `if (!isMaster && !addressedToSelf && !isBroadcastTarget) { ... }` body)
// into a pure classification. Every check/comparison below is unchanged from
// the original — only the hop-limit-exceeded cases now return
// DropHopLimitExceeded instead of directly `return`-ing out of
// processAdapterData; the caller's switch (Mesh::processAdapterData) turns
// that back into an unconditional early return, preserving the original
// drop-the-whole-frame behavior exactly (see the header doc comment).
RouteDecision DownlinkRouter::classify(const mesh_message& msg, const uint8_t* deviceMac,
                                       bool isMaster, bool addressedToSelf, bool isBroadcastTarget,
                                       bool addressedToMaster, uint8_t* nextHopMacOut) const {
  if (isMaster || addressedToSelf || isBroadcastTarget)
    return RouteDecision::NotRouted;
  if (addressedToMaster) {
    // Uplink: relay toward master via routing table.
    if (msg.hop_count >= lattice::config::MAX_HOPS)
      return RouteDecision::DropHopLimitExceeded;
    return RouteDecision::RelayTowardMaster;
  }
  // Downlink toward a specific node. If the frame carries a source route and
  // we are on it, forward to the next hop (stateless — spec §4); otherwise
  // fall back to the flood.
  if (msg.route_len > 0 && msg.route_len <= lattice::config::MAX_HOPS) {
    for (uint8_t i = 0; i < msg.route_len; ++i) {
      if (lattice::mac::eq(&msg.route_path[static_cast<size_t>(i) * 6], deviceMac)) {
        if (msg.hop_count >= lattice::config::MAX_HOPS)
          return RouteDecision::DropHopLimitExceeded;
        const uint8_t* next = (i + 1 < msg.route_len)
                                  ? &msg.route_path[static_cast<size_t>(i + 1) * 6]
                                  : msg.target_mac_address;
        memcpy(nextHopMacOut, next, 6);
        return RouteDecision::ForwardOnRoute;
      }
    }
  }
  return RouteDecision::Flood;
}

void DownlinkRouter::relayDownlink(const mesh_message& msg, const PeerRegistry& peers,
                                   const uint8_t* deviceMac, MeshTransport& transport) {
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMac, 6);
  for (const auto& p : peers) {
    if (lattice::mac::eq(p.mac, deviceMac))
      continue;
    transport.sendMessage(p.mac, relay, deviceMac);
  }
}

void DownlinkRouter::registerDownlinkPeer(const uint8_t* mac, const PeerRegistry& peers,
                                          const MasterInfo& currentMaster) {
  // Enrolled peers and the current master are managed exclusively by their
  // own paths (PeerRegistry / enrollment) — just register (idempotent) and
  // never track or evict them via this LRU.
  bool isCurrentMaster = currentMaster.distance != 0xFF && lattice::mac::eq(mac, currentMaster.mac);
  if (peers.find(mac) || isCurrentMaster) {
    // Defense-in-depth (issue #47 item 5): if this MAC was already parked in
    // the downlink forwarding-peer LRU from earlier churn (before it became
    // enrolled or the current master), evict it here — its peering is now
    // owned by PeerRegistry/enrollment, not this LRU. This branch is taken on
    // every call once a MAC is enrolled/master (it short-circuits ahead of
    // the LRU-touch loop below), so without this eviction a stale entry
    // would sit in downlinkPeerLru indefinitely instead of freeing its slot.
    for (size_t i = 0; i < downlinkPeerLruCount; ++i) {
      if (lattice::mac::eq(downlinkPeerLru[i], mac)) {
        for (size_t j = i; j + 1 < downlinkPeerLruCount; ++j)
          memcpy(downlinkPeerLru[j], downlinkPeerLru[j + 1], 6);
        downlinkPeerLruCount--;
        break;
      }
    }
    MeshTransport::registerPeerWithEspNow(mac);
    return;
  }

  // Already tracked: touch (move to front) and ensure still registered.
  for (size_t i = 0; i < downlinkPeerLruCount; ++i) {
    if (lattice::mac::eq(downlinkPeerLru[i], mac)) {
      uint8_t touched[6];
      memcpy(touched, downlinkPeerLru[i], 6);
      for (size_t j = i; j > 0; --j)
        memcpy(downlinkPeerLru[j], downlinkPeerLru[j - 1], 6);
      memcpy(downlinkPeerLru[0], touched, 6);
      MeshTransport::registerPeerWithEspNow(mac);
      return;
    }
  }

  // Not tracked. Evict the oldest (LRU) entry from ESP-NOW first if at
  // capacity (spec §2: "20-peer cap, LRU-evicted") — otherwise an RF attacker
  // crafting downlink frames with fresh distinct next-hop MACs on every frame
  // would grow this set unbounded and eventually exhaust the ESP-NOW peer
  // table (no self-heal, no reboot).
  if (downlinkPeerLruCount >= lattice::config::LATTICE_DOWNLINK_PEER_MAX) {
    uint8_t* oldest = downlinkPeerLru[downlinkPeerLruCount - 1];
    if (esp_now_is_peer_exist(oldest))
      esp_now_del_peer(oldest);
  } else {
    ++downlinkPeerLruCount;
  }
  for (size_t j = downlinkPeerLruCount - 1; j > 0; --j)
    memcpy(downlinkPeerLru[j], downlinkPeerLru[j - 1], 6);
  memcpy(downlinkPeerLru[0], mac, 6);
  MeshTransport::registerPeerWithEspNow(mac);
}

} // namespace mesh
} // namespace lattice
