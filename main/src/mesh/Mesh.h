#ifndef MESH_H
#define MESH_H

#include <functional>
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include <array>
#include <cstdint>
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

static constexpr uint8_t PROTO_VERSION = 3;

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

  void readMacAddress();
  void printMac(const uint8_t* mac);
  void printMeshMessage(const mesh_message& msg);

  static void onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status);
  void IRAM_ATTR onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData,
                                    int len);
  static void IRAM_ATTR dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data,
                                           int len);

  mesh_message buildMessage(adapter_types type, const uint8_t* data, MeshMessageType msgType);

  std::function<void(const mesh_message&)> externalRecvCallback;

  MasterInfo currentMaster;
  bool isMaster;
  uint32_t lastBeaconMillis;
  uint32_t lastMasterBeaconReceivedMs;
  static constexpr uint32_t STALE_MASTER_THRESHOLD_MS = lattice::config::STALE_MASTER_THRESHOLD_MS;

  // Peer routing (uses currentMaster — stays in Mesh)
  PeerInfo* findNextHopToMaster();

  void sendMessage(const uint8_t* target, const mesh_message& msg);
  void broadcastToAllPeers(const mesh_message& msg);

  void transmitCore(const adapter_types type, const uint8_t* data,
                    MeshMessageType msgType = MESH_TYPE_ADAPTER_DATA,
                    const mesh_message* msgOverride = nullptr);

  // Shared body for transmit()/transmitSelfOriginated() — see their comments.
  void transmitDispatch(const adapter_types type, const uint8_t* data, bool selfOriginated);

  void loadMeshKeyFromEEPROM();
  void saveMeshKeyToEEPROM(const uint8_t* key);
  void generateRandomMeshKey();
  bool meshKeyIsSet() const;

  // --- Tiger Style refactor helpers ---
  void processMasterBeacon(const mesh_message& msg);
  void processAdapterData(const mesh_message& msg);
  void relayDownlink(const mesh_message& msg);
  // Relay an enrollment (JOIN_REQUEST) broadcast one hop toward the master so a
  // node out of direct RF range of the master can still enroll (Task 9b Bug #5).
  void relayEnrollmentUplink(const mesh_message& msg);

  // Setup helpers (Tiger Style refactor)
  bool setupWiFi();
  bool setupEspNow();
  void loadPersistentState();

  // Enrollment helper (relay dispatch only — "addressed to us" branch is in Enrollment)
  void processJoinAck(const mesh_message& msg);

  // Add (or key-update) a peer in the registry, persist, and register it with
  // ESP-NOW encryption. Shared by enrollPeer() (master registers the enrolling
  // node; allowRekey=true — the hub-approved serial path may legitimately
  // re-key a re-enrolling node) and processJoinAck() (node registers the
  // approving master; allowRekey=false — an over-the-air JOIN_ACK must never
  // replace established key material, only set it on first contact or upgrade
  // a placeholder all-zero key). Returns false if the registry is full and the
  // peer could not be added.
  bool registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey);

  // Replay protection (composed)
  ReplayCache replay;

  // Single choke point for drawing a tx sequence number from replay.txSeqNum.
  // ALL sites that need a fresh (epoch, seq) pair for a message we originate
  // MUST go through this — it is the only place that guards against the
  // 0xFFFF -> 0 wrap (spec §2): a reused (epoch, seq) pair after a silent
  // wrap would reuse an AEAD nonce. On wrap, bumps + persists the boot epoch
  // before redrawing so the new sequence starts under a fresh epoch.
  uint16_t nextSeqGuarded();

#ifdef UNIT_TEST
  ReplayCache& testReplay() { return replay; }
  NeighborTable& testNeighbors() { return neighbors; }
  RouteTable& testRoutes() { return routes; }
  uint32_t testMillisNow() { return millis(); } // exposes the node's mocked clock to tests
  const uint8_t* testDeviceMac() const { return deviceMacAddress; }
#endif

  // Relay jitter: deferred relay pending fields (Task 3)
  mesh_message relayPendingMsg;
  uint32_t relayPendingAt;
  bool relayPending;

  bool _dualMasterMode;

  // --- ESP-NOW receive ring buffer (lock-free SPSC) ---
  static constexpr size_t RECV_QUEUE_SIZE = 8;

  struct RecvQueueEntry {
    uint8_t srcMac[6];
    mesh_message msg;
  };

  RecvQueueEntry recvQueue[RECV_QUEUE_SIZE];
  volatile uint8_t recvQueueHead; // written by WiFi task (callback)
  uint8_t recvQueueTail;          // read by main task (loop)

  void drainRecvQueue();
  bool sendRouteReport();
  void processRouteReport(const mesh_message& msg);

  // Beacon timer (moved from broadcastMasterBeacon for loop() integration)
  uint32_t lastBeaconMs;
  uint32_t lastRouteReportMs;

  // Enrollment state (composed — mbedtls-heavy methods stubbed in test builds)
  Enrollment enrollment;

  // E2E AEAD (spec §1/§2): per-peer derived key cache + lookup helpers.
  E2EKeyStore e2eKeys;
  // Forwarding candidates toward the master, learned from overheard master
  // beacons (spec §3). Routing only — never consulted for E2E crypto.
  NeighborTable neighbors;
  // Master-side node -> relay path store, populated from route reports
  // (spec §4), consulted for downlink source routing.
  RouteTable routes;
  // Holds a NeighborTable-resolved next hop (not an enrolled peer) so
  // findNextHopToMaster() can return a stable PeerInfo* for a pure relay,
  // which is never added to `peers` (enrollment-only rule).
  PeerInfo nextHopScratch{};
  // MAC of the single auto-registered (unencrypted) ESP-NOW peer currently
  // standing in for a multi-hop forwarding relay, or all-zero if none. A node
  // only ever forwards uplink to ONE next hop at a time, so at most one such
  // peer is ever needed — bounding it here closes the ESP-NOW peer table
  // exhaustion vector (spec §2: "20-peer cap, LRU-evicted") that an RF
  // attacker flooding distinct-MAC spoofed beacons would otherwise trigger.
  // Enrolled peers (master + sensors, held in `peers`) are NEVER tracked or
  // evicted here.
  uint8_t forwardingPeer[6]{};
  // Returns k_up/k_down for the current master (leaf side); false if not enrolled
  // or master pubkey unknown.
  bool masterE2EKeys(const uint8_t** kUp, const uint8_t** kDown);
  // Returns keys for an enrolled origin peer (master side); false if unknown peer.
  bool peerE2EKeys(const uint8_t* originMac, const uint8_t** kUp, const uint8_t** kDown);
  static bool isSealedType(uint8_t messageType);

public:
  Mesh();
  bool init();

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

  // Node role config
  void setIsMaster(bool value) { isMaster = value; }
  bool getIsMaster() const { return isMaster; }
  void setDualMasterMode(bool value) { _dualMasterMode = value; }
  bool getDualMasterMode() const { return _dualMasterMode; }

  // Peer management API (optional, can be used in your app/UI)
  void addPeer(const uint8_t* mac);
  void removePeer(const uint8_t* mac) { peers.removeAndPersist(mac); }
  const PeerInfo* getPeerList() const { return peers.peerMacs; }
  size_t getPeerCount() const { return peers.peerCount; }

  // Broadcast adapter data to all peers.
  // deliverLocally: also hand the built message to externalRecvCallback, the
  // same delivery path used for messages received from the mesh. Needed so
  // master-originated, server-bound data (currently: health reports) reaches
  // this node's own serial adapter — broadcastToAllPeers() explicitly skips
  // self, so without this the master could never answer for itself.
  void broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally = false);

  // Master-only: source-route a sealed downlink to a specific enrolled node
  // (spec §4). Seals `data` with the destination's k_down, then unicasts via
  // the reversed relay path recorded in RouteTable (from that node's most
  // recent route report), or broadcast-floods if no route is known. No-op if
  // this node is not master. See Mesh.cpp for the full rationale.
  void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data);

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
    // Draw seq via the guarded choke point FIRST (it may bump replay.bootEpoch
    // on wrap), then read bootEpoch — reading it as a separate statement after
    // the draw (rather than in the same call as replay.nextSeq()) avoids
    // depending on unspecified argument evaluation order for a possibly-mutated
    // member.
    uint16_t seq = nextSeqGuarded();
    enrollment.sendRequest(deviceMacAddress, PROTO_VERSION, replay.bootEpoch, seq);
  }
  bool isEnrolled() const { return enrollment.isEnrolled(); }
  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32);

  // Enrollment relay callback — set by Serial_Adapter owner (main.ino)
  void setEnrollmentRelayFn(EnrollmentRelayFn fn) { enrollment.setRelayFn(fn); }

  // Get current hop count to master (0 if this node is master)
  uint8_t getHopCount() const { return isMaster ? 0 : currentMaster.distance; }

#if SIMULATE_MODE
  // Inject a message directly into the receive queue (bypasses radio — for dev/test only)
  void injectReceivedMessage(const uint8_t* srcMac, const mesh_message& msg) {
    uint8_t nextHead = (recvQueueHead + 1) % RECV_QUEUE_SIZE;
    if (nextHead == recvQueueTail)
      return; // Queue full — drop
    RecvQueueEntry& slot = recvQueue[recvQueueHead];
    memcpy(slot.srcMac, srcMac, 6);
    slot.msg = msg;
    recvQueueHead = nextHead;
  }
#endif
};

} // namespace mesh
} // namespace lattice

#endif // MESH_H
