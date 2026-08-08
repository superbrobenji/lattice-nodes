#pragma once
#include <cstddef>
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"
#include "MeshTransport.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

// Outcome of DownlinkRouter::classify() below. NotRouted means "not a
// downlink-routing case at all — fall through to the security gate/local
// delivery path unchanged" (self-addressed, broadcast-target, or this node is
// master). DropHopLimitExceeded is distinct from NotRouted: it means "this
// WAS a routing case (addressed to master, or on the frame's source route),
// but hop_count is already at MAX_HOPS" — the original code drops the frame
// outright in this case (an unconditional return from the whole
// processAdapterData function), never reaching the security gate. See the
// classify() doc comment below for why collapsing this into NotRouted would
// be a correctness bug.
enum class RouteDecision {
  NotRouted,
  RelayTowardMaster,
  ForwardOnRoute,
  Flood,
  DropHopLimitExceeded
};

// Phase B Task 6 (finding 1 job 4, narrowed; finding 2's routing half): owns
// downlink relay, auto-peer-registration for forwarding, and the routing
// *decision* for processAdapterData's downlink half — NOT the security/E2E
// half, which stays on Mesh (see Mesh::processAdapterData's untouched
// security gate -> E2E open -> config-opcode authorization -> deliver
// sequence). Extracted out of Mesh; every check and early-return point below
// is unchanged from the original Mesh::relayDownlink/registerDownlinkPeer and
// processAdapterData's routing block — only which class owns the code
// changed. See docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md,
// Task 6.
//
// Stays crypto-free like MasterBeacon/MeshTransport: classify() is read-only
// (const, no I/O), and relayDownlink()/registerDownlinkPeer() take whatever
// external state (PeerRegistry, MasterInfo, MeshTransport) they need as
// parameters rather than holding a back-reference to Mesh. The
// crypto-touching relay-toward-master action (transmitCore, which needs
// lattice::mesh::masterE2EKeys (E2EKeyLookup.h) and txState.checkEpochRollback)
// stays a Mesh-executed action driven by classify()'s result, not a
// DownlinkRouter method.
class DownlinkRouter {
public:
  // Read-only classification of processAdapterData's downlink routing
  // decision — no state mutation, no I/O. nextHopMacOut is written only when
  // the result is ForwardOnRoute.
  //
  // DropHopLimitExceeded vs NotRouted: the original code's
  // `if (msg.hop_count >= MAX_HOPS) return;` inside the addressedToMaster and
  // route-path-match branches is an unconditional early-return from the
  // WHOLE processAdapterData function — the frame is dropped outright, never
  // reaching the security gate below. NotRouted, by contrast, is meant to
  // fall through to that security/local-delivery path (see the caller's
  // switch). Mapping the hop-limit case to NotRouted would therefore change
  // behavior — a hop-limit-exceeded frame would wrongly reach local
  // delivery/security processing instead of being dropped. DropHopLimitExceeded
  // preserves the original drop-the-whole-frame semantics exactly.
  RouteDecision classify(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                         bool addressedToSelf, bool isBroadcastTarget, bool addressedToMaster,
                         uint8_t nextHopMacOut[6]) const;

  // Was Mesh::relayDownlink — moved verbatim except sendMessage(...) ->
  // transport.sendMessage(...). Flood-fallback broadcast relay to every
  // known peer except deviceMac (self).
  void relayDownlink(const mesh_message& msg, const PeerRegistry& peers, const uint8_t* deviceMac,
                     MeshTransport& transport);

  // Was Mesh::registerDownlinkPeer — moved verbatim except
  // registerPeerWithEspNow(...) -> MeshTransport::registerPeerWithEspNow(...)
  // (static, per Task 4), and downlinkPeerLru/downlinkPeerLruCount below are
  // now this class's own private members instead of Mesh's.
  void registerDownlinkPeer(const uint8_t* mac, const PeerRegistry& peers,
                            const MasterInfo& currentMaster);

#ifdef UNIT_TEST
  // In unit test builds, all members are public so test bodies (which live in
  // compiler-generated subclasses of the fixture and therefore cannot inherit
  // C++ friend access) can access private state directly. Mirrors Mesh.h's/
  // MeshTransport.h's/MasterBeacon.h's own UNIT_TEST toggle.
public:
#else
private:
#endif
  // Bounded LRU of auto-registered DOWNLINK forwarding-peer MACs (spec §2:
  // "20-peer cap, LRU-evicted"). Unlike Mesh's single uplink forwardingPeer
  // (which stays on Mesh — uplink routing, not this class's job), a
  // node/master may legitimately need to have MULTIPLE distinct downlink
  // forwarding peers registered concurrently (e.g. relaying several in-flight
  // source-routed frames toward different next hops), so this is a small
  // fixed-capacity LRU rather than a single slot. Capacity is
  // LATTICE_DOWNLINK_PEER_MAX (project_config.h) — kept small so enrolled
  // peers + Mesh's uplink forwardingPeer + this stay well under the ~20
  // ESP-NOW cap. Entries are ordered most-recently-used first (index 0).
  // Enrolled peers (PeerRegistry) and the current master are NEVER tracked or
  // evicted here — see registerDownlinkPeer().
  uint8_t downlinkPeerLru[lattice::config::LATTICE_DOWNLINK_PEER_MAX][6]{};
  size_t downlinkPeerLruCount{0};
};

} // namespace mesh
} // namespace lattice
