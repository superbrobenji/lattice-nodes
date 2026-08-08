#ifndef MESH_H
#define MESH_H

#include <functional>
#include <esp_now.h>
#include <esp_attr.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <array>
#include <cstdint>
#include <memory>
#include "src/adapter/Adapter.h"
#include "src/persistence/EepromManager.h"
#include "../../project_config.h" // Added for global limits/config
#include "../../lib/lattice-protocol/c/message_types.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "ReplayCache.h"
#include "PeerRegistry.h"
#include "Enrollment.h"
#include "E2EKeyStore.h"
#include "NeighborTable.h"
#include "RouteTable.h"
#include "MeshTransport.h"
#include "MasterBeacon.h"
#include "DownlinkRouter.h"
#include "E2EKeyLookup.h"
#include "PeerEnrollment.h"
#include "UplinkRouter.h"
#include "MeshMessenger.h"
#include "RouteReportHandler.h"
#include "FrameAuthorizer.h"

#ifdef UNIT_TEST
// Forward declarations for test fixture classes (global namespace) so that
// friend declarations inside lattice::mesh::Mesh are valid.
class ReplayCacheTest;
class MeshLogicTest;
#endif

namespace lattice {
namespace mesh {

using ::mesh_message;
using ::MeshMessageType;
using lattice::adapter::adapter_types;

// PROTO_VERSION lives in MeshMessenger.h (single definition, fix round 1 —
// see that header's comment) since Mesh.h already #includes it above.

class Mesh {
#ifdef UNIT_TEST
  // In unit test builds, all members are public so test bodies (which live in
  // compiler-generated subclasses of the fixture and therefore cannot inherit
  // C++ friend access) can access private state directly.
public:
#else
private:
#endif
  static constexpr int MESH_KEY_SIZE = 16;

  uint8_t meshKey[MESH_KEY_SIZE];

  static Mesh* instance;

  uint8_t deviceMacAddress[6];
  uint8_t lastSeenMasterMac[6];

  esp_now_peer_info_t peerInfo;

  PeerRegistry peers; // Peer list management (no heap alloc)

  // ESP-NOW radio setup, RX ring buffer + trampoline + drain, and send
  // primitives (Phase B Task 4, finding 1 job 1; finding 19). Mesh delegates
  // all radio I/O to this instead of owning it directly.
  MeshTransport transport;

  void readMacAddress();

  std::function<void(const mesh_message&)> externalRecvCallback;

  MasterInfo currentMaster;
  bool isMaster;

  // Master-role beacon broadcast timing, master-timeout detection, and
  // incoming-beacon processing (TOFU master-MAC learning, dual-master
  // failover, duplicate-beacon-relay suppression) — Phase B Task 5 (finding 1
  // job 3). Owns what were lastBeaconMillis/lastMasterBeaconReceivedMs/
  // STALE_MASTER_THRESHOLD_MS as its own private members.
  MasterBeacon beacon;

  // Downlink relay, auto-peer-registration for forwarding, and the
  // routing-decision half of processAdapterData's downlink branch (Phase B
  // Task 6, finding 1 job 4 narrowed; finding 2's routing half). The
  // security/E2E half of processAdapterData now lives in frameAuthorizer
  // (round 2 task 13) — see FrameAuthorizer.h's doc comment.
  // Mesh::processAdapterData switches on router.classify()'s result and
  // executes the crypto-touching relay-toward-master action itself
  // (transmitCore needs lattice::mesh::masterE2EKeys (E2EKeyLookup.h), and
  // txState.checkEpochRollback).
  DownlinkRouter router;

  // Uplink mirror of DownlinkRouter (round 2 task 10): owns findNextHopToMaster
  // and the forwardingPeer LRU-of-one bookkeeping that used to live directly on
  // Mesh — see UplinkRouter.h.
  UplinkRouter uplinkRouter;

  // Owns outbound message construction and dispatch (round 2 task 11) --
  // buildMessage/transmitCore/transmitDispatch/broadcastAdapterData/
  // sendDownlinkToNode/enrollPeer/relayEnrollmentUplink all moved here. Mesh's
  // remaining methods of the same names are thin forwards into this — see
  // MeshMessenger.h.
  MeshMessenger messenger;

  // Route-report protocol handling — send + process, including the chain-MAC
  // verification (issue #44 route-path-forgery defense) that used to live
  // directly on Mesh (round 2 task 12). See RouteReportHandler.h.
  RouteReportHandler routeReportHandler;

  // processAdapterData's security half (round 2 task 13, the plan's most
  // security-sensitive extraction) — master-not-self-addressed sealed-type
  // gate, E2E open both directions, config-opcode authorization. Moved
  // verbatim from Mesh; see FrameAuthorizer.h's doc comment for why the E2E
  // open stays bundled with the authorization decision rather than being
  // split out further. Mesh keeps only local-delivery dispatch after this
  // returns.
  FrameAuthorizer frameAuthorizer;

  void loadMeshKeyFromEEPROM();

  // --- Tiger Style refactor helpers ---
  void processAdapterData(const mesh_message& msg);

  // Setup helpers (Tiger Style refactor)
  // Wraps transport.setup() (Wi-Fi bring-up) plus this node's own MAC-address
  // ownership (readMacAddress()/peers.setDeviceMac()) — MAC address stays on
  // Mesh (Phase B Task 4) since many other things beyond transport need it.
  bool setupRadio();
  void loadPersistentState();

  // Enrollment helper (relay dispatch only — "addressed to us" branch is in Enrollment)
  void processJoinAck(const mesh_message& msg);

  // Static trampoline binding lattice::mesh::registerPeerWithKey(allowRekey=false)
  // to Enrollment::RegisterPeerFn's plain-function-pointer signature (item H) —
  // routes through the singleton `instance` the same way dataRecvTrampoline
  // does for esp_now_register_recv_cb. Used only by processJoinAck() (via
  // lattice::mesh::dispatchJoinAck, round 2 task 10).
  static bool registerPeerWithKeyTrampoline(const uint8_t* mac, const uint8_t* publicKey32) {
    return lattice::mesh::registerPeerWithKey(mac, publicKey32, /*allowRekey=*/false,
                                              instance->peers, instance->enrollment,
                                              instance->_dualMasterMode);
  }

  // Replay protection (composed)
  ReplayCache replay;

  // This node's own outbound sequence + relay-dedup bookkeeping (finding 15
  // split — was fields directly on ReplayCache). Round 2 Task 8: txState now
  // also owns the guarded-sequence-draw and seal-time epoch-rollback guard
  // (txState.nextSeqGuarded()/txState.checkEpochRollback()) — see
  // OutboundSequenceState in ReplayCache.h for both.
  OutboundSequenceState txState;

#ifdef UNIT_TEST
  ReplayCache& testReplay() { return replay; }
  OutboundSequenceState& testTxState() { return txState; }
  NeighborTable& testNeighbors() { return neighbors; }
  RouteTable* testRoutes() { return routes.get(); }
  // Exposes the node's mocked clock to tests. Phase I Task 6 (FF): return
  // type widened uint32_t -> uint64_t to match esp_timer_get_time()/1000ULL.
  uint64_t testMillisNow() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL; }
  const uint8_t* testDeviceMac() const { return deviceMacAddress; }
#endif

  // Relay jitter: deferred relay pending fields (Task 3)
  mesh_message relayPendingMsg;
  // Phase I Task 6 (FF): widened uint32_t -> uint64_t (absolute deadline in ms).
  uint64_t relayPendingAt;
  bool relayPending;

  bool _dualMasterMode;

  // Dispatch for one message dequeued by transport.drain() (Phase B Task 4).
  // Reproduces the old Mesh::drainRecvQueue's post-pop body exactly: proto-
  // version check, replay check, peers.updateLastSeen, then the message-type
  // switch into Mesh-owned handlers (enrollment.processRequest,
  // beacon.process, processAdapterData, etc.) — this dispatch stays on Mesh
  // because MeshTransport has no visibility into those collaborators.
  void handleReceivedMessage(const uint8_t srcMac[6], const mesh_message& msg);

  // Static trampoline binding handleReceivedMessage to
  // MeshTransport::MessageHandler's plain-function-pointer signature — routes
  // through the singleton `instance` the same way dataRecvTrampoline used to.
  // Used only by drain() below.
  static void handleReceivedMessageTrampoline(const uint8_t srcMac[6], const mesh_message& msg) {
    if (instance)
      instance->handleReceivedMessage(srcMac, msg);
  }

  // Beacon timer (moved from broadcastMasterBeacon for loop() integration).
  // lastBeaconMs is currently dead (unused outside its zero-init in the
  // constructor) — pre-existing, out of Task 6's scope — retyped for
  // consistency with the rest of this FF sweep rather than left a stale
  // uint32_t.
  uint64_t lastBeaconMs;
  uint64_t lastRouteReportMs;

  // Enrollment state (composed — mbedtls-heavy methods stubbed in test builds)
  Enrollment enrollment;

  // E2E AEAD (spec §1/§2): per-peer derived key cache + lookup helpers.
  E2EKeyStore e2eKeys;
  // Forwarding candidates toward the master, learned from overheard master
  // beacons (spec §3). Routing only — never consulted for E2E crypto.
  NeighborTable neighbors;
  // Master-side node -> relay path store, populated from route reports
  // (spec §4), consulted for downlink source routing. Allocated only when
  // this node is a master (issue #51) — see reevaluateRouteTable(). A
  // unique_ptr (not a raw pointer + hand-written Mesh destructor) so Mesh
  // keeps its compiler-generated move constructor — a user-declared
  // destructor would suppress it, and Mesh's copy constructor is already
  // implicitly deleted (RouteTable/NeighborTable/E2EKeyStore each delete
  // theirs), so move is the only thing letting Mesh be returned by value
  // from the test factory helpers (see those types' header comments).
  std::unique_ptr<RouteTable> routes;

public:
  Mesh();
  // No user-declared destructor here (deliberately — see `routes` member
  // comment below): one would suppress Mesh's implicit move constructor,
  // which is the only thing that lets Mesh be returned by value from the
  // test factory helpers (RouteTable/NeighborTable/E2EKeyStore already
  // delete their own copy ctor for the same reason, so Mesh's copy ctor is
  // already implicitly deleted and it depends entirely on move).
  bool init();

  // Re-evaluate whether this node needs a RouteTable, honouring the current
  // isMaster flag. Called from init() and on live role changes (issue #51 —
  // RouteTable is ~2.25 KB static RAM that leaves never use).
  void reevaluateRouteTable() {
    if (isMaster && !routes)
      routes = std::make_unique<RouteTable>();
    if (!isMaster && routes)
      routes.reset();
    // Phase G audit item B: role-split the E2E derived-key cache the same way —
    // leaves only ever need one slot per master (primary + secondary), masters
    // need one per enrolled node. E2EKeyStore defaults to the master size so
    // standalone construction (unit tests) keeps working unchanged; this shrinks
    // it for leaves once the real role is known.
    e2eKeys.setCapacity(isMaster ? lattice::config::LATTICE_E2E_KEYCACHE_MAX
                                 : lattice::config::LATTICE_E2E_KEYCACHE_MAX_LEAF);
  }

  // Static trampoline for Adapter usage. NOTE: keep this exact 2-arg
  // signature — it's assigned by address to Adapter::TransmitPtr
  // (mesh_transmit_fn), which is a plain function pointer type; adding a
  // (even defaulted) parameter here changes that pointer's type and breaks
  // that assignment. Forwarding/relay callers (e.g. relaying server-issued
  // commands onward through the mesh) should use this.
  static void transmit(const adapter_types type, const uint8_t* data);

  // Use instead of transmit() when this node is originating data ABOUT
  // itself that must reach the server (currently: the master's own health
  // report). On a master node, transmit() only reaches OTHER mesh peers —
  // broadcastToAllPeers() explicitly skips self — so self-originated data
  // would otherwise never reach the server. This delivers the built message
  // locally via externalRecvCallback in addition to the normal broadcast.
  static void transmitSelfOriginated(const adapter_types type, const uint8_t* data);

  void linkDataRecvCallback(std::function<void(const mesh_message&)> recvCallback);

  // Master beacon: call in main loop if node is master; handles timing internally
  void broadcastMasterBeacon();

  // Master timeout check: call in main loop; clears stale master route on timeout
  void checkMasterTimeout();

  // Drain pending work queued from ISR/callback contexts (call from main loop())
  void loop();

  // Phase I Task 9 (item EE): drain the ESP-NOW receive ring buffer
  // (recvQueue) until empty, dispatching each entry the same way
  // drainRecvQueue() always has. Previously drainRecvQueue() ran inline as
  // the first statement of loop(); it now runs from a dedicated FreeRTOS task
  // (main.cpp's mesh_task_fn) woken by xTaskNotifyWait() when
  // onDataRecvCallback's ISR trampoline signals drainNotifyHandle_ — see
  // setDrainNotifyHandle() below. loop() no longer drains recvQueue itself.
  // Public (unlike drainRecvQueue) so main.cpp's task body and the host
  // unit-test / SimNode harness — which has no real FreeRTOS task — can both
  // call it directly.
  void drain() { transport.drain(&Mesh::handleReceivedMessageTrampoline); }

  // Registers the dedicated mesh task's handle so onDataRecvCallback's ISR
  // trampoline can wake it via vTaskNotifyGiveFromISR after enqueueing into
  // recvQueue. Call once from main.cpp's setup(), after the task is created.
  // A null handle (the default, and the state throughout host/SimNode tests)
  // is valid — onDataRecvCallback null-checks before notifying, so the ISR
  // path simply skips the notify and drain() must instead be driven directly
  // (as it is by SimNode::tick() and the unit tests).
  void setDrainNotifyHandle(TaskHandle_t handle) { transport.setDrainNotifyHandle(handle); }

  // Node role config
  // Re-evaluates the RouteTable allocation (issue #51) on every role change,
  // not just from init(): both main.cpp and the e2e SimNode harness call
  // setIsMaster() AFTER init() (to apply the persisted/dev-mode master flag
  // once EEPROM/adapter setup has run), so relying on init() alone would
  // leave a real master's routes permanently nullptr. reevaluateRouteTable()
  // is a cheap no-op when the role hasn't actually changed.
  void setIsMaster(bool value) {
    isMaster = value;
    reevaluateRouteTable();
  }
  bool getIsMaster() const { return isMaster; }
  void setDualMasterMode(bool value) { _dualMasterMode = value; }
  bool getDualMasterMode() const { return _dualMasterMode; }

  // Peer management API (optional, can be used in your app/UI)
  void addPeer(const uint8_t* mac) { lattice::mesh::addPeer(mac, peers); }
  void removePeer(const uint8_t* mac) { peers.removeAndPersist(mac); }
  const PeerInfo* getPeerList() const { return peers.begin(); }
  size_t getPeerCount() const { return peers.count(); }

  // Broadcast adapter data to all peers.
  // deliverLocally: also hand the built message to externalRecvCallback, the
  // same delivery path used for messages received from the mesh. Needed so
  // master-originated, server-bound data (currently: health reports) reaches
  // this node's own serial adapter — broadcastToAllPeers() explicitly skips
  // self, so without this the master could never answer for itself.
  // Thin forward (round 2 task 11) — body now lives in MeshMessenger.
  void broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally = false) {
    messenger.broadcastAdapterData(type, data, deliverLocally, deviceMacAddress, currentMaster,
                                   txState, peers, transport, externalRecvCallback);
  }

  // Master-only: source-route a sealed downlink to a specific enrolled node
  // (spec §4). Seals `data` with the destination's k_down, then unicasts via
  // the reversed relay path recorded in RouteTable (from that node's most
  // recent route report), or broadcast-floods if no route is known. No-op if
  // this node is not master. See MeshMessenger.cpp for the full rationale.
  // Thin forward (round 2 task 11) — body now lives in MeshMessenger.
  void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data) {
    messenger.sendDownlinkToNode(destMac, type, data, isMaster, deviceMacAddress, currentMaster,
                                 txState, peers, enrollment, e2eKeys, routes.get(), router,
                                 transport);
  }

  // Serial adapter helper (optional broadcast)
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t* data);

  // Serial adapter helper: static shim to sendDownlinkToNode (mirrors
  // broadcastAdapterDataStatic above) so SerialAdapter can source-route+seal a
  // targeted server command without holding a Mesh instance itself.
  static void sendDownlinkToNodeStatic(const uint8_t* destMac, adapter_types type,
                                       const uint8_t* data);

  // Debug helper
  void debugDumpRadio();

  // Provisioning: public key accessor (private key never exposed)
  const uint8_t* getDevicePublicKey() const { return enrollment.getPublicKey(); }

  // Singleton accessor (used by Serial_Adapter for enrollment callbacks)
  static Mesh* getInstance() { return instance; }

  // Enrollment protocol
  void sendEnrollmentRequest() {
    // Pass proto_version + a fresh (epoch, seq) so ReplayCache can dedup relayed
    // copies of this request while still allowing the deliberate 10s retry.
    // Draw seq via the guarded choke point FIRST (it may bump txState.bootEpoch
    // on wrap), then read bootEpoch — reading it as a separate statement after
    // the draw (rather than in the same call as txState.nextSeq()) avoids
    // depending on unspecified argument evaluation order for a possibly-mutated
    // member.
    uint16_t seq = txState.nextSeqGuarded();
    enrollment.sendRequest(deviceMacAddress, PROTO_VERSION, txState.bootEpoch, seq);
  }
  bool isEnrolled() const { return enrollment.isEnrolled(); }
  // Thin forward (round 2 task 11) — body now lives in MeshMessenger.
  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32) {
    enrollPeer(mac, publicKey32, nullptr, nullptr);
  }
  // 4-arg overload: also stamps the server-provided secondary-master identity
  // (secondaryMac/secondaryPubKey32) into the JOIN_ACK broadcast to the newly
  // enrolled node, so it can TOFU-learn its failover master up front (Phase 4).
  // Pass nullptr, nullptr (as the 2-arg overload does) when there is no
  // secondary — the ACK's secondary fields are then left zeroed. Thin forward
  // (round 2 task 11) — body now lives in MeshMessenger.
  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                  const uint8_t* secondaryPubKey32) {
    messenger.enrollPeer(mac, publicKey32, secondaryMac, secondaryPubKey32, deviceMacAddress,
                         txState, peers, enrollment, _dualMasterMode, transport);
  }

  // Enrollment relay callback — set by Serial_Adapter owner (main.ino)
  void setEnrollmentRelayFn(EnrollmentRelayFn fn) { enrollment.setRelayFn(fn); }

  // Get current hop count to master (0 if this node is master)
  uint8_t getHopCount() const { return isMaster ? 0 : currentMaster.distance; }

#if SIMULATE_MODE
  // Inject a message directly into the receive queue (bypasses radio — for
  // dev/test only). Forwards to MeshTransport, which owns recvQueue (Phase B
  // Task 4).
  void injectReceivedMessage(const uint8_t* srcMac, const mesh_message& msg) {
    transport.injectReceivedMessage(srcMac, msg);
  }
#endif
};

} // namespace mesh
} // namespace lattice

#endif // MESH_H
