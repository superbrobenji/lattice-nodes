#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"
#include "Enrollment.h"
#include "MeshTransport.h"
#include "NeighborTable.h"
#include "PeerRegistry.h"
#include "ReplayCache.h"

namespace lattice {
namespace mesh {

// Phase B Task 5 (finding 1 job 3): owns master-role beacon broadcast timing,
// master-timeout detection, and incoming-beacon processing — TOFU master-MAC
// learning (Enrollment::learnMasterMac/learnSecondaryMasterMac), dual-master
// failover, distance/freshness tracking via NeighborTable, and
// duplicate-beacon-relay suppression (OutboundSequenceState::wasRelayedBefore/
// markRelayed). Extracted out of Mesh — every check, comparison operator, and
// early-return point below is unchanged from the original
// Mesh::broadcastMasterBeacon/checkMasterTimeout/processMasterBeacon; only
// which class owns the code changed. See
// docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md, Task 5.
//
// Deliberately holds no `instance`/singleton pattern (unlike MeshTransport,
// which needs one to serve as a static C-callback target) — nothing calls
// into MasterBeacon from a callback, so Mesh always reaches it through its
// own `beacon` member.
class MasterBeacon {
public:
  // True at most once per MASTER_BEACON_INTERVAL_MS — was the
  // `now - lastBeaconMillis < MASTER_BEACON_INTERVAL_MS` guard inline in the
  // old Mesh::broadcastMasterBeacon. Updates lastBeaconMillis as a side
  // effect on true, matching the original's timing semantics exactly.
  bool intervalElapsed();

  // Sends a pre-built beacon message broadcast. Mesh builds the message via
  // buildMessage (a Mesh-owned method touching crypto/sequencing state
  // MasterBeacon has no need of) — this just times + sends it.
  void send(const mesh_message& msg, MeshTransport& transport);

  // Was Mesh::checkMasterTimeout — moved verbatim. isMaster/currentMaster/
  // lastSeenMasterMac are threaded through as parameters;
  // lastMasterBeaconReceivedMs/STALE_MASTER_THRESHOLD_MS stay MasterBeacon's
  // own private members, shared with process() below (both need the same
  // field).
  void checkTimeout(bool isMaster, MasterInfo& currentMaster, uint8_t lastSeenMasterMac[6]);

  // Was Mesh::processMasterBeacon — moved verbatim. enrollment/neighbors/
  // currentMaster/txState are collaborators Mesh already owns, threaded
  // through as references. relayPendingMsgOut/relayPendingAtOut/
  // relayPendingOut are Mesh-owned output fields consumed by Mesh::loop()'s
  // deferred-relay dispatch — unrelated to beacon processing itself beyond
  // being written here.
  void process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
               bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
               MasterInfo& currentMaster, OutboundSequenceState& txState,
               mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut, bool& relayPendingOut,
               uint8_t lastSeenMasterMac[6]);

#ifdef UNIT_TEST
  // In unit test builds, all members are public so test bodies (which live in
  // compiler-generated subclasses of the fixture and therefore cannot inherit
  // C++ friend access) can access private state directly. Mirrors Mesh.h's/
  // MeshTransport.h's own UNIT_TEST toggle.
public:
#else
private:
#endif
  uint64_t lastBeaconMillis{0};
  uint64_t lastMasterBeaconReceivedMs{0};
  static constexpr uint32_t STALE_MASTER_THRESHOLD_MS = lattice::config::STALE_MASTER_THRESHOLD_MS;
};

} // namespace mesh
} // namespace lattice
