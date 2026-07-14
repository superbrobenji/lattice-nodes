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

static constexpr uint8_t PROTO_VERSION = 2;

// Enrollment relay callback — registered by Serial_Adapter owner (main.ino).
// Called from loop() when a pending enrollment is ready to relay to the server.
typedef void (*EnrollmentRelayFn)(const uint8_t mac[6], const uint8_t pubKey[32]);

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
  void printMac(const uint8_t mac[6]);
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

  void sendMessage(const uint8_t target[6], mesh_message msg);
  void broadcastToAllPeers(mesh_message msg);

  void transmitCore(const adapter_types type, const uint8_t* data,
                    MeshMessageType msgType = MESH_TYPE_ADAPTER_DATA,
                    const mesh_message* msgOverride = nullptr);

  void loadMeshKeyFromEEPROM();
  void saveMeshKeyToEEPROM(const uint8_t* key);
  void generateRandomMeshKey();
  bool meshKeyIsSet() const;

  // --- Tiger Style refactor helpers ---
  void processMasterBeacon(const mesh_message& msg);
  void processAdapterData(const mesh_message& msg);
  void relayDownlink(const mesh_message& msg);

  // Setup helpers (Tiger Style refactor)
  bool setupWiFi();
  bool setupEspNow();
  void loadPersistentState();

  // Enrollment helpers
  void processEnrollmentRequest(const mesh_message& msg);
  void processJoinAck(const mesh_message& msg);

  // Curve25519 keypair
  uint8_t devicePrivateKey[32];
  uint8_t devicePublicKey[32];
  void loadOrGenerateKeypair();

  // Replay protection (composed)
  ReplayCache replay;

  // Relay jitter: deferred relay pending fields (Task 3)
  mesh_message relayPendingMsg;
  uint32_t relayPendingAt;
  bool relayPending;

  // TOFU master MAC — learned on first enrollment beacon, persisted across reboots
  uint8_t knownMasterMac[6];
  bool hasMasterMac;
  uint8_t knownMasterMacSecondary[6];
  bool hasMasterMacSecondary;
  bool _dualMasterMode;

  // Pending enrollment relay (filled in ESP-NOW callback, drained in loop())
  volatile bool _pendingEnrollmentRelay = false;
  uint8_t _pendingEnrollmentMac[6]{};
  uint8_t _pendingEnrollmentPubKey[32]{};
  EnrollmentRelayFn _enrollmentRelayFn = nullptr;

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
  void drainPendingEnrollment();
  bool sendRouteReport();
  void processRouteReport(const mesh_message& msg);

  // Beacon timer (moved from broadcastMasterBeacon for loop() integration)
  uint32_t lastBeaconMs;
  uint32_t lastRouteReportMs;

public:
  Mesh();
  bool init();

  // Static trampoline for Adapter usage
  static void transmit(const adapter_types type, const uint8_t* data);

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
  void addPeer(const uint8_t mac[6]) { peers.addAndPersist(mac); }
  void removePeer(const uint8_t mac[6]) { peers.removeAndPersist(mac); }
  const PeerInfo* getPeerList() const { return peers.peerMacs; }
  size_t getPeerCount() const { return peers.peerCount; }

  // Broadcast adapter data to all peers
  void broadcastAdapterData(adapter_types type, const uint8_t* data);

  // Serial adapter helper (optional broadcast)
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t* data);

  // Debug helper
  void debugDumpRadio();

  // Provisioning: public key accessor (private key never exposed)
  const uint8_t* getDevicePublicKey() const { return devicePublicKey; }

  // Singleton accessor (used by Serial_Adapter for enrollment callbacks)
  static Mesh* getInstance() { return instance; }

  // Enrollment protocol
  void sendEnrollmentRequest();
  bool isEnrolled() const;
  void enrollPeer(const uint8_t mac[6], const uint8_t publicKey32[32]);

  // Enrollment relay callback — set by Serial_Adapter owner (main.ino)
  void setEnrollmentRelayFn(EnrollmentRelayFn fn);

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
