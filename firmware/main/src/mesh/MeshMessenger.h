#pragma once
#include <cstdint>
#include <functional>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../lib/lattice-protocol/c/message_types.h"
#include "../../project_config.h"
#include "src/adapter/Adapter.h"
#include "DownlinkRouter.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "MeshTransport.h"
#include "NeighborTable.h"
#include "PeerRegistry.h"
#include "ReplayCache.h"
#include "RouteTable.h"
#include "UplinkRouter.h"

namespace lattice {
namespace mesh {

using ::mesh_message;
using ::MeshMessageType;
using lattice::adapter::adapter_types;

// Terminal local-delivery callback shape — matches Mesh's existing
// externalRecvCallback member type (std::function<void(const mesh_message&)>).
// Named here (round 2 task 11) since MeshMessenger::broadcastAdapterData is
// the first collaborator that needs it threaded through as a parameter;
// Task 12/13's collaborators reuse the same typedef.
using ExternalRecvCallback = std::function<void(const mesh_message&)>;

// Protocol v0.6.0 flag-day (Phase G §8 wire shrink): 4→5, bumped atomically
// with hub Task 6 in a parallel PR — must merge together. Single definition
// (fix round 1): this used to also exist as a separate Mesh::PROTO_VERSION-
// shaped class constant on MeshMessenger, which risked a future version bump
// touching one copy and not the other — a wire-compat split. Mesh.h no
// longer declares its own copy; it consumes this one (Mesh.h #includes this
// header already).
static constexpr uint8_t PROTO_VERSION = 5;

// Message types whose payload is E2E-sealed (spec §1/§2) — decides both
// "should this frame get sealed on send" (MeshMessenger::transmitCore) and
// "should this frame be opened on receive" (FrameAuthorizer::authorize, round
// 2 task 13 — formerly Mesh::processAdapterData directly, before that
// function's security half moved out).
// Single definition (fix round 1): used to also exist as a second,
// hand-inlined copy of this same two-message-type check inside
// transmitCore, which risked a future third sealed message type being added
// to one copy and missed in the other — a silent send/receive divergence,
// not a compile error. A free function (not a MeshMessenger method) since
// FrameAuthorizer needs it too and holds no MeshMessenger reference.
inline bool isSealedType(uint8_t messageType) {
  return messageType == MESH_TYPE_ADAPTER_DATA || messageType == MESH_TYPE_ROUTE_REPORT;
}

// Owns outbound message construction and dispatch (round 2 task 11) — the
// single place "how does this node send something" lives. Depends on more
// collaborators than any other class in this plan, by design: everything a
// send needs to thread through (sequencing, E2E crypto, uplink routing,
// downlink source-routing) converges here. Every method takes the
// collaborators it needs as explicit parameters rather than holding a Mesh&
// back-pointer — same no-back-pointer discipline every other extracted
// collaborator in this plan follows (DownlinkRouter, UplinkRouter,
// MeshTransport, E2EKeyLookup.h, PeerEnrollment.h). See
// docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md, Task 11.
class MeshMessenger {
public:
  mesh_message buildMessage(adapter_types type, const uint8_t* data, MeshMessageType msgType,
                            const uint8_t* deviceMac, const MasterInfo& currentMaster,
                            OutboundSequenceState& txState);

  void transmitCore(const adapter_types type, const uint8_t* data, MeshMessageType msgType,
                    const mesh_message* msgOverride, bool isMaster, const uint8_t* deviceMac,
                    MasterInfo& currentMaster, OutboundSequenceState& txState, PeerRegistry& peers,
                    Enrollment& enrollment, E2EKeyStore& e2eKeys, UplinkRouter& uplinkRouter,
                    NeighborTable& neighbors, MeshTransport& transport);

  // Shared body for Mesh::transmit()/transmitSelfOriginated() — both stay on
  // Mesh as static trampolines (Adapter holds a plain function-pointer member
  // — see Mesh.h's Adapter::TransmitPtr comment) and forward into this.
  void transmitDispatch(const adapter_types type, const uint8_t* data, bool selfOriginated,
                        bool isMaster, ExternalRecvCallback externalRecvCallback,
                        const uint8_t* deviceMac, MasterInfo& currentMaster,
                        OutboundSequenceState& txState, PeerRegistry& peers, Enrollment& enrollment,
                        E2EKeyStore& e2eKeys, UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                        MeshTransport& transport);

  void broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally,
                            const uint8_t* deviceMac, const MasterInfo& currentMaster,
                            OutboundSequenceState& txState, PeerRegistry& peers,
                            MeshTransport& transport, ExternalRecvCallback externalRecvCallback);

  // `router` (DownlinkRouter&) is not in the design doc's original Task 11
  // sketch — the body's router.registerDownlinkPeer(...) call (auto-register
  // the first hop of a source-routed downlink as an ESP-NOW peer) needs it;
  // added here since the moved body genuinely calls it (see
  // MeshMessenger.cpp's sendDownlinkToNode).
  void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data,
                          bool isMaster, const uint8_t* deviceMac, MasterInfo& currentMaster,
                          OutboundSequenceState& txState, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, RouteTable* routes,
                          DownlinkRouter& router, MeshTransport& transport);

  // 2-arg convenience overload (no secondary-master identity) — forwards to
  // the 4-arg overload below with nullptr, nullptr, mirroring the shape of
  // Mesh::enrollPeer's own 2-arg/4-arg split.
  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* deviceMac,
                  OutboundSequenceState& txState, PeerRegistry& peers, Enrollment& enrollment,
                  bool dualMasterMode, MeshTransport& transport);

  // `peers`/`dualMasterMode` are not in the design doc's original Task 11
  // sketch — the body's lattice::mesh::registerPeerWithKey(...) call needs
  // both; added here since the moved body genuinely calls it (see
  // MeshMessenger.cpp).
  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                  const uint8_t* secondaryPubKey32, const uint8_t* deviceMac,
                  OutboundSequenceState& txState, PeerRegistry& peers, Enrollment& enrollment,
                  bool dualMasterMode, MeshTransport& transport);

  // Relay an enrollment (JOIN_REQUEST) broadcast one hop toward the master so a
  // node out of direct RF range of the master can still enroll (Task 9b Bug #5).
  void relayEnrollmentUplink(const mesh_message& msg, const uint8_t* deviceMac,
                             MasterInfo& currentMaster, OutboundSequenceState& txState,
                             PeerRegistry& peers, Enrollment& enrollment, E2EKeyStore& e2eKeys,
                             UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                             MeshTransport& transport);
};

} // namespace mesh
} // namespace lattice
