#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "MeshMessenger.h"
#include "PeerRegistry.h"
#include "RouteTable.h"
#include "UplinkRouter.h"

namespace lattice {
namespace mesh {

// Route-report protocol handling — send + process, including chain-MAC
// verification (issue #44) and E2E open/seal. Moved verbatim from Mesh (round 2
// task 12); same protected-security-code discipline as Mesh's remaining
// processAdapterData half (task 13) and DownlinkRouter's hop-limit distinction.
class RouteReportHandler {
public:
  bool sendRouteReport(bool isMaster, UplinkRouter& uplinkRouter, MasterInfo& currentMaster,
                       PeerRegistry& peers, NeighborTable& neighbors, Enrollment& enrollment,
                       E2EKeyStore& e2eKeys, const uint8_t* deviceMac,
                       OutboundSequenceState& txState, MeshMessenger& messenger,
                       MeshTransport& transport);

  // uplinkRouter/neighbors are not in the brief's original signature sketch —
  // the relay branch's messenger.transmitCore(...) call (moved verbatim from
  // Mesh::processRouteReport) needs both; added here since the moved body
  // genuinely calls it, same rationale as MeshMessenger::sendDownlinkToNode's
  // DownlinkRouter& addition (task 11).
  void processRouteReport(const mesh_message& msg, bool isMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, RouteTable* routes,
                          const uint8_t* deviceMac, MasterInfo& currentMaster,
                          OutboundSequenceState& txState, MeshMessenger& messenger,
                          UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                          MeshTransport& transport, ExternalRecvCallback externalRecvCallback);
};

} // namespace mesh
} // namespace lattice
