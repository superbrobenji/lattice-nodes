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

  // Serial adapter helper (optional broadcast)
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t* data);

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
    enrollment.sendRequest(deviceMacAddress, PROTO_VERSION, replay.bootEpoch, replay.nextSeq());
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
