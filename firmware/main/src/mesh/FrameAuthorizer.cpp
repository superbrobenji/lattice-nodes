#include "FrameAuthorizer.h"
#include "E2ECrypto.h"
#include "E2EKeyLookup.h"
#include "MeshMessenger.h"
#include "src/logging/Logger.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "src/network/MacEq.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

AuthResult FrameAuthorizer::authorize(const mesh_message& msg, bool isMaster, bool addressedToSelf,
                                      const MasterInfo& currentMaster, PeerRegistry& peers,
                                      Enrollment& enrollment, E2EKeyStore& e2eKeys,
                                      mesh_message& openedOut) {
  // Security gate: at the master, a sealed-type frame (ADAPTER_DATA/ROUTE_REPORT)
  // that is NOT addressed to self must never reach local delivery unopened. No
  // leaf ever originates a broadcast-target (FF:FF:FF:FF:FF:FF) sealed uplink —
  // only the master's own downlink broadcast (broadcastAdapterData, which delivers
  // locally directly and never re-enters this function) and beacons use FF:FF. So
  // a broadcast-target (or otherwise not-self-addressed) sealed frame arriving here
  // over the air at the master is either a stale self-echo or a forgery — drop it
  // rather than deliver it to externalRecvCallback without E2E authentication.
  if (isMaster && !addressedToSelf && lattice::mesh::isSealedType(msg.message_type)) {
    LATTICE_LOGLN("MESH",
                  "Master: sealed-type frame not addressed to self rejected (unauthenticated)",
                  LogLevel::LOG_WARN);
    return AuthResult::Rejected;
  }

  // Local delivery
  // E2E open (spec §2): master unseals self-targeted uplink before local delivery.
  mesh_message opened = msg;
  bool needsOpen = isMaster && addressedToSelf && lattice::mesh::isSealedType(msg.message_type);
  if (needsOpen) {
    const uint8_t *kUp, *kDown;
    if (!lattice::mesh::peerE2EKeys(msg.origin_mac_address, peers, enrollment, e2eKeys, &kUp,
                                    &kDown) ||
        !lattice::mesh::crypto::openPayload(kUp, opened)) {
      LATTICE_LOGLN("MESH", "E2E open failed — frame dropped", LogLevel::LOG_WARN);
      return AuthResult::Rejected;
    }
  }

  // Node-side E2E open (spec §2): a self-addressed sealed ADAPTER_DATA from the
  // master is opened with our k_down before local delivery. Mirrors the master's
  // uplink open above. Failure → drop (finding-#9 pattern). Broadcast (FF:FF)
  // frames are NOT opened — addressedToSelf is false for those, so this never
  // fires for them (they stay plaintext, handled below).
  bool nodeOpened = false;
  if (!isMaster && addressedToSelf && msg.message_type == MESH_TYPE_ADAPTER_DATA) {
    const uint8_t *kUp, *kDown;
    if (!lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown) ||
        !lattice::mesh::crypto::openPayload(kDown, opened)) {
      LATTICE_LOGLN("MESH", "downlink open failed — dropped", LogLevel::LOG_WARN);
      return AuthResult::Rejected;
    }
    nodeOpened = true;
  }

  bool isConfigOpcode = (opened.data_type == adapter_types::SERIAL_ADAPTER &&
                         (opened.data[0] == OP_CONFIG_SET || opened.data[0] == OP_NODE_ID_SET));
  // Critical fix: config opcodes (CONFIG_SET / NODE_ID_SET) are state-changing
  // (adapter-type reconfig + restart, node-identity assignment) and must be
  // honored ONLY via the sealed, opened path above (needsOpen on the master,
  // nodeOpened on a node) — never via a broadcast-target or otherwise-unopened
  // frame. Without this, a forged plaintext BROADCAST (target FF:FF)
  // ADAPTER_DATA frame is never addressedToSelf, so it is never opened; since
  // origin_mac is attacker-controlled and the master's real MAC is public in
  // beacons, such a frame sailed past the origin check below too and reached
  // externalRecvCallback fully unauthenticated (one plaintext RF frame could
  // reboot/reconfigure any node). Legitimate non-config broadcast adapter data
  // (e.g. OP_HEALTH_REQ, OP_TX_POWER_SET) is unaffected — this guard only
  // fires for CONFIG_SET/NODE_ID_SET.
  if (isConfigOpcode && !needsOpen && !nodeOpened) {
    LATTICE_LOGLN("MESH", "Config opcode via unopened/broadcast path rejected (unauthenticated)",
                  LogLevel::LOG_WARN);
    return AuthResult::Rejected;
  }
  // enrollment.hasMasterMac/knownMasterMac accessed via the public
  // hasKnownMaster()/knownMaster() accessors here, not the raw private fields
  // Mesh.cpp reads directly — Enrollment only grants `friend class Mesh;`
  // (Enrollment.h), so FrameAuthorizer (new code, round 2 task 13) uses the
  // same accessor API MasterBeacon/DownlinkRouter already established rather
  // than growing the friend list. Reads are identical; only the access path
  // differs from the original inline block.
  if (isConfigOpcode && enrollment.hasKnownMaster()) {
    bool fromPrimary = lattice::mac::eq(opened.origin_mac_address, enrollment.knownMaster());
    bool fromSecondary =
        enrollment.hasKnownSecondaryMaster() &&
        lattice::mac::eq(opened.origin_mac_address, enrollment.knownSecondaryMaster());
    if (!fromPrimary && !fromSecondary) {
      LATTICE_LOGLN("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
      return AuthResult::Rejected;
    }
  }
  // Note: the "master received ADAPTER_DATA not addressed to self" case is now
  // handled (and rejected) by the security gate above — ADAPTER_DATA is always a
  // sealed type, so isMaster && !addressedToSelf never reaches this point.
  openedOut = opened;
  return AuthResult::Authorized;
}

} // namespace mesh
} // namespace lattice
