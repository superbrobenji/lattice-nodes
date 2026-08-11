#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

enum class AuthResult { Rejected, Authorized };

// Authorization decision + E2E open for an inbound ADAPTER_DATA frame (round 2 task
// 13) -- moved verbatim from Mesh::processAdapterData's security half. The E2E-open
// step is part of the authorization question (an unopened, still-sealed frame isn't
// yet known-authentic), not separable the way DownlinkRouter's crypto-free routing
// decision was. Mesh keeps only local-delivery dispatch after this returns.
class FrameAuthorizer {
public:
  // openedOut is written only when the result is Authorized. currentMaster is not
  // in the design sketch's original signature but is required verbatim by the
  // node-side E2E open branch's lattice::mesh::masterE2EKeys(...) call (same
  // rationale as RouteReportHandler's currentMaster addition, task 12).
  AuthResult authorize(const mesh_message& msg, bool isMaster, bool addressedToSelf,
                       const MasterInfo& currentMaster, PeerRegistry& peers, Enrollment& enrollment,
                       E2EKeyStore& e2eKeys, mesh_message& openedOut);
};

} // namespace mesh
} // namespace lattice
