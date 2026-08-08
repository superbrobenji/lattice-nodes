#include "MeshMessenger.h"
#include "E2ECrypto.h"
#include "E2EKeyLookup.h"
#include "PeerEnrollment.h"
#include "RouteMac.h"
#include "src/logging/Logger.h"
#include "src/network/MacEq.h"
#include <esp_timer.h>
#include <cstring>

namespace lattice {
namespace mesh {

mesh_message MeshMessenger::buildMessage(adapter_types type, const uint8_t* data,
                                         MeshMessageType msgType, const uint8_t* deviceMac,
                                         const MasterInfo& currentMaster,
                                         OutboundSequenceState& txState) {
  mesh_message msg = {};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = msgType;
  msg.data_type = type;
  memcpy(msg.origin_mac_address, deviceMac, 6);
  if (msgType == MESH_TYPE_MASTER_BEACON) {
    memset(msg.target_mac_address, 0xFF, 6); // Not used
  } else {
    memcpy(msg.target_mac_address, currentMaster.mac, 6);
  }
  memcpy(msg.last_hop_mac_address, deviceMac, 6);
  if (data)
    memcpy(msg.data, data, sizeof(msg.data));
  msg.hop_count = 0;
  msg.seq_num = txState.nextSeqGuarded();
  msg.epoch_num = txState.bootEpoch;
  return msg;
}

void MeshMessenger::transmitCore(const adapter_types type, const uint8_t* data,
                                 MeshMessageType msgType, const mesh_message* msgOverride,
                                 bool isMaster, const uint8_t* deviceMac, MasterInfo& currentMaster,
                                 OutboundSequenceState& txState, PeerRegistry& peers,
                                 Enrollment& enrollment, E2EKeyStore& e2eKeys,
                                 UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                                 MeshTransport& transport) {
  mesh_message msg;
  if (msgOverride) {
    msg = *msgOverride;
  } else {
    msg = buildMessage(type, data, msgType, deviceMac, currentMaster, txState);
  }

  bool selfOriginated = (lattice::mac::eq(msg.origin_mac_address, deviceMac));

  // Only a self-originated uplink sets its own target to the master. A relayed
  // frame (msgOverride, foreign origin) is already sealed against the origin's
  // target — rewriting it would corrupt the AEAD AAD the destination master
  // verifies. Leave relayed frames' target untouched.
  if (msgType == MESH_TYPE_ADAPTER_DATA && selfOriginated) {
    memcpy(msg.target_mac_address, currentMaster.mac, 6);
  }

  // E2E seal (spec §1/§2): self-originated uplink payloads only. Relayed frames
  // (msgOverride with foreign origin) are already sealed — forward untouched.
  if (!isMaster && selfOriginated && lattice::mesh::isSealedType(msg.message_type)) {
    const uint8_t *kUp, *kDown;
    txState.checkEpochRollback(msg.epoch_num, msg.seq_num);
    if (!lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown) ||
        !lattice::mesh::crypto::sealPayload(kUp, msg)) {
      LATTICE_LOGLN("MESH", "E2E seal unavailable — uplink dropped", LogLevel::LOG_WARN);
      return;
    }

    // Chain-MAC seed (Phase C, spec §4 / issue #44): a self-originated route
    // report seeds msg.auth_path with this node's own hop in the chain (hop
    // 0 — prev_hop zeroed, no hop precedes the origin). Relays extend the
    // chain as they append to route_path (processRouteReport's relay
    // branch); the master reconstructs and verifies before recording the
    // route (processRouteReport's master branch). Reuses kUp already
    // derived above for the seal — same pairwise k_up with the master, no
    // new key material. Scoped to ROUTE_REPORT only: ADAPTER_DATA frames
    // don't carry a route_path to authenticate (see design doc non-goals —
    // no downlink-frame MAC).
    if (msg.message_type == MESH_TYPE_ROUTE_REPORT) {
      uint8_t prev_hop[6] = {0};
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, deviceMac, ctx);
      uint8_t zero_prev_mac[routemac::AUTH_PATH_LEN] = {0};
      routemac::chainStep(kUp, ctx, zero_prev_mac, msg.auth_path);
    }
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop =
      uplinkRouter.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac,
                                       static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL);
  if (nextHop && !lattice::mac::eq(nextHop->mac, deviceMac)) {
    transport.sendMessage(nextHop->mac, msg, deviceMac);
  } else {
    // No route to master is a routine, self-healing transient: a node that has
    // just booted (or whose master went stale) legitimately has no next hop
    // until it hears the next beacon. Drop the frame quietly rather than
    // escalating to err::fail — escalation here drives the error LED and
    // reboot-reason tracking, and turns every such gap (see
    // docs/design-gaps/multihop-data-uplink.md) into an error loop instead of a
    // silent drop. The upstream sender retries on its own timer.
    LATTICE_LOGLN("MESH", "No next hop to master — message dropped. Master timeout or unreachable.",
                  LogLevel::LOG_WARN);
  }
}

void MeshMessenger::transmitDispatch(const adapter_types type, const uint8_t* data,
                                     bool selfOriginated, bool isMaster,
                                     ExternalRecvCallback externalRecvCallback,
                                     const uint8_t* deviceMac, MasterInfo& currentMaster,
                                     OutboundSequenceState& txState, PeerRegistry& peers,
                                     Enrollment& enrollment, E2EKeyStore& e2eKeys,
                                     UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                                     MeshTransport& transport) {
  if (isMaster) {
    broadcastAdapterData(type, data, selfOriginated, deviceMac, currentMaster, txState, peers,
                         transport, externalRecvCallback);
    return;
  }
  transmitCore(type, data, MESH_TYPE_ADAPTER_DATA, nullptr, isMaster, deviceMac, currentMaster,
               txState, peers, enrollment, e2eKeys, uplinkRouter, neighbors, transport);
}

void MeshMessenger::broadcastAdapterData(adapter_types type, const uint8_t* data,
                                         bool deliverLocally, const uint8_t* deviceMac,
                                         const MasterInfo& currentMaster,
                                         OutboundSequenceState& txState, PeerRegistry& peers,
                                         MeshTransport& transport,
                                         ExternalRecvCallback externalRecvCallback) {
  mesh_message msg =
      buildMessage(type, data, MESH_TYPE_ADAPTER_DATA, deviceMac, currentMaster, txState);
  memset(msg.target_mac_address, 0xFF, 6); // broadcast indicator — relayed by intermediate nodes
  transport.broadcastToAllPeers(msg, peers, deviceMac);
  if (deliverLocally && externalRecvCallback) {
    externalRecvCallback(msg);
  }
}

// Defense-in-depth (issue #47 item 4): true when a route path length would
// overflow route_path[]/MAX_HOPS bounds. RouteTable::record() already clamps
// pathLen at write time (parse-safety: `if (pathLen > config::MAX_HOPS) return;`
// in RouteTable.h), so this branch is not reachable via any current
// legitimate call path into sendDownlinkToNode() — routes->lookup() can only
// ever hand back a pathLen that record() previously accepted. The check below
// stays local to sendDownlinkToNode rather than relying solely on
// RouteTable's own guard, so the bound survives a future refactor of either
// side. Pure/stack-only (no allocation) — a free function (external linkage,
// not a MeshMessenger member) so it stays directly unit-testable without
// needing to drive an integration path around RouteTable's guard, which is
// otherwise unreachable from outside RouteTable.h. Moved verbatim from
// Mesh.cpp (round 2 task 11) alongside sendDownlinkToNode, its only caller.
bool downlinkRouteLenExceedsMaxHops(uint8_t pathLen) {
  return pathLen > lattice::config::MAX_HOPS;
}

void MeshMessenger::sendDownlinkToNode(const uint8_t* destMac, adapter_types type,
                                       const uint8_t* data, bool isMaster, const uint8_t* deviceMac,
                                       MasterInfo& currentMaster, OutboundSequenceState& txState,
                                       PeerRegistry& peers, Enrollment& enrollment,
                                       E2EKeyStore& e2eKeys, RouteTable* routes,
                                       DownlinkRouter& router, MeshTransport& transport) {
  if (!isMaster)
    return;
  mesh_message msg =
      buildMessage(type, data, MESH_TYPE_ADAPTER_DATA, deviceMac, currentMaster, txState);
  memcpy(msg.target_mac_address, destMac, 6); // AAD-bound destination — set before sealing

  const uint8_t *kUp, *kDown;
  txState.checkEpochRollback(msg.epoch_num, msg.seq_num);
  if (!lattice::mesh::peerE2EKeys(destMac, peers, enrollment, e2eKeys, &kUp, &kDown) ||
      !lattice::mesh::crypto::sealPayload(kDown, msg)) {
    LATTICE_LOGLN("MESH", "downlink seal unavailable — dropped", LogLevel::LOG_WARN);
    return;
  }

  uint8_t path[lattice::config::MAX_HOPS * 6];
  uint8_t pathLen = 0;
  if (routes && routes->lookup(destMac, path, &pathLen) && pathLen > 0) {
    // Defensive clamp (issue #47 item 4) before indexing path[]/msg.route_path
    // with pathLen below — see downlinkRouteLenExceedsMaxHops() above.
    if (downlinkRouteLenExceedsMaxHops(pathLen)) {
      LATTICE_LOGLN("MESH", "downlink route_len exceeds MAX_HOPS — dropping", LogLevel::LOG_ERROR);
      return;
    }
    // RouteTable stores the path in origin->master order (as accumulated by
    // relays on the uplink route report); reverse it into master->origin order
    // for the downlink source route.
    msg.route_len = pathLen;
    for (uint8_t i = 0; i < pathLen; ++i)
      memcpy(&msg.route_path[static_cast<size_t>(i) * 6],
             &path[static_cast<size_t>(pathLen - 1 - i) * 6], 6);
    // First hop = route_path[0]; auto-register it as an unencrypted ESP-NOW peer
    // so esp_now_send can unicast to it — real ESP-NOW requires the peer to be
    // registered first (VirtualBus doesn't enforce this, but the Phase-2 lesson
    // was that skipping it here is a real-hardware bug). Bounded via the
    // downlink forwarding-peer LRU (spec §2) — see DownlinkRouter::registerDownlinkPeer().
    router.registerDownlinkPeer(msg.route_path, peers, currentMaster);
    transport.sendMessage(msg.route_path, msg, deviceMac);
    return;
  }
  // No known multi-hop route: fall back to broadcast flood (still sealed).
  // Direct/adjacent nodes and unknown-route nodes are reached this way.
  msg.route_len = 0;
  transport.broadcastToAllPeers(msg, peers, deviceMac);
}

void MeshMessenger::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32,
                               const uint8_t* deviceMac, OutboundSequenceState& txState,
                               PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode,
                               MeshTransport& transport) {
  enrollPeer(mac, publicKey32, nullptr, nullptr, deviceMac, txState, peers, enrollment,
             dualMasterMode, transport);
}

void MeshMessenger::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32,
                               const uint8_t* secondaryMac, const uint8_t* secondaryPubKey32,
                               const uint8_t* deviceMac, OutboundSequenceState& txState,
                               PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode,
                               MeshTransport& transport) {
  if (!lattice::mesh::registerPeerWithKey(mac, publicKey32, /*allowRekey=*/true, peers, enrollment,
                                          dualMasterMode))
    return; // registry full — do not ACK an enrollment we could not record

  // Send JOIN_ACK unicast to new node
  mesh_message ack = {};
  // Stamp proto_version + (epoch, seq) so the existing ReplayCache dedups
  // re-broadcast copies of this ACK (Task 9c R2): each relay node re-broadcasts a
  // given JOIN_ACK at most once (the reflected copy is dropped by isReplay before
  // processJoinAck), preventing combinatorial broadcast amplification.
  ack.proto_version = PROTO_VERSION;
  // Draw seq via the guarded choke point FIRST — it may bump txState.bootEpoch
  // on wrap — then stamp epoch_num from the (possibly just-bumped) value so
  // the ACK's epoch always matches the epoch its seq_num was drawn under.
  ack.seq_num = txState.nextSeqGuarded();
  ack.epoch_num = txState.bootEpoch;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  ack.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(ack.origin_mac_address, deviceMac, 6);
  memcpy(ack.target_mac_address, mac, 6);
  memcpy(ack.last_hop_mac_address, deviceMac, 6);
  ack.hop_count = 0;
  // Include first 4 bytes of approved node's pubkey as fingerprint
  memcpy(ack.data, publicKey32, 4);
  // Include OUR public key so the enrolling node can register this master as
  // an encrypted, routable peer in its own registry (see Enrollment::processJoinAck).
  memcpy(ack.enrollment_public_key, enrollment.getPublicKey(), 32);
  // Stamp the server-provided secondary-master identity, if any, so the
  // enrolling node can TOFU-learn its failover master from this same ACK
  // (Phase 4). Protocol v0.6.0 (wire shrink §8) packs this into the JOIN_ACK
  // data[] payload rather than top-level MeshMessage fields:
  //   data[0..4]   = node pubkey fingerprint (set above)
  //   data[4..10]  = secondaryMasterMac
  //   data[10..42] = secondaryPublicKey
  //   data[42..64] = zero
  // Left zeroed (ack's default) when there is no secondary.
  if (secondaryMac && secondaryPubKey32) {
    memcpy(ack.data + 4, secondaryMac, 6);
    memcpy(ack.data + 10, secondaryPubKey32, 32);
  }
  // Broadcast via the registered FF:FF:… peer so the new node receives the ACK
  // even before it is individually registered as a unicast peer.
  transport.sendBroadcast(ack);
  LATTICE_LOGLN("MESH", "JOIN_ACK sent to newly enrolled node", LogLevel::LOG_INFO);
}

void MeshMessenger::relayEnrollmentUplink(const mesh_message& msg, const uint8_t* deviceMac,
                                          MasterInfo& currentMaster, OutboundSequenceState& txState,
                                          PeerRegistry& peers, Enrollment& enrollment,
                                          E2EKeyStore& e2eKeys, UplinkRouter& uplinkRouter,
                                          NeighborTable& neighbors, MeshTransport& transport) {
  // Never relay our own outbound request echoed back to us over the air.
  if (lattice::mac::eq(msg.origin_mac_address, deviceMac))
    return;
  // Bound relay depth (mirrors the ADAPTER_DATA uplink guard).
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  // Can only relay toward the master if we actually have a route to it.
  if (!uplinkRouter.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac,
                                        static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL))
    return;
  // Relay one hop toward the master, exactly like the ADAPTER_DATA uplink path:
  // bump hop_count, stamp ourselves as last hop, and route via uplinkRouter.findNextHopToMaster
  // (transmitCore does NOT rewrite target for non-ADAPTER_DATA types, so the
  // request's broadcast target is preserved for the master to process).
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMac, 6);
  // isMaster is hardcoded false here: relayEnrollmentUplink is only ever
  // reached from Mesh::handleReceivedMessage's MESH_TYPE_ENROLLMENT case, in
  // the `!isMaster` branch (a master calls enrollment.processRequest(msg)
  // instead) — mirrors the original Mesh::relayEnrollmentUplink, an instance
  // method that only ever ran with this->isMaster == false.
  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ENROLLMENT,
               &relay,
               /*isMaster=*/false, deviceMac, currentMaster, txState, peers, enrollment, e2eKeys,
               uplinkRouter, neighbors, transport);
}

} // namespace mesh
} // namespace lattice
