#ifndef MESH_H
#define MESH_H

#include <functional>
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include <array>
#include <cstdint>
#include "src/Adapter/Adapter.h"
#include "src/persistence/EEPROM_Manager.h"
#include "../../project_config.h" // Added for global limits/config

#ifdef UNIT_TEST
// Forward declarations for test fixture classes (global namespace) so that
// friend declarations inside lattice::mesh::Mesh are valid.
class ReplayCacheTest;
class MeshLogicTest;
#endif

namespace lattice {
namespace mesh {

using lattice::adapter::adapter_types;
using lattice::utils::EEPROM_SIZES::MAX_PEERS; // Use constant from EEPROM_Manager

// --- Mesh protocol message type ---
enum MeshMessageType : uint8_t {
  MESH_TYPE_ADAPTER_DATA = 0,
  MESH_TYPE_MASTER_BEACON = 1,
  MESH_TYPE_ENROLLMENT = 2,
  MESH_TYPE_SERIAL_CMD_BROADCAST = 3, // server→device only
  MESH_TYPE_JOIN_ACK =
      4, // server→device only; was 3, changed to avoid collision with SERIAL_CMD_BROADCAST
};

static constexpr uint8_t PROTO_VERSION = 2;

// --- Mesh message struct (packed: wire protocol, no padding) ---
struct __attribute__((packed)) mesh_message {
  uint8_t protoVersion; // Always PROTO_VERSION (2)
  MeshMessageType messageType;
  adapter_types dataType;
  uint8_t originMacAddress[6];
  uint8_t targetMacAddress[6];
  uint8_t lastHopMacAddress[6];
  uint8_t data[64];
  uint8_t hopCount;
  uint32_t epochNum;               // Boot count of origin node (replay protection)
  uint16_t seqNum;                 // Per-boot message counter (replay protection)
  uint8_t enrollmentPublicKey[32]; // Curve25519 key; zero for non-enrollment messages
};
// 1+1+4+6+6+6+64+1+4+2+32 = 127 bytes (adapter_types is int32_t = 4B, packed)
static_assert(sizeof(mesh_message) == 127, "mesh_message size changed — update server proto");

// Peer info struct for RAM and EEPROM storage
struct PeerInfo {
  uint8_t mac[6];
  uint8_t publicKey[32]; // Curve25519 public key (zero = not yet known)
  uint32_t lastSeenMillis;
};

// Master routing info
struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance;   // Hops to master
  uint8_t nextHop[6]; // Next hop MAC
};

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

  PeerInfo peerMacs[MAX_PEERS]; // Fixed-size peer list (no heap alloc)
  size_t peerCount;             // Number of valid entries in peerMacs

  void readMacAddress();
  void printMac(const uint8_t mac[6]);
  void printMeshMessage(const mesh_message& msg);

  static void onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status);
  void IRAM_ATTR onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData,
                                    int len);
  static void IRAM_ATTR dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data,
                                           int len);

  mesh_message buildMessage(adapter_types type, const uint8_t* data, MeshMessageType msgType);

  std::function<void(mesh_message)> externalRecvCallback;

  MasterInfo currentMaster;
  bool isMaster;
  uint32_t lastBeaconMillis;
  uint32_t lastMasterBeaconReceivedMs;
  static constexpr uint32_t STALE_MASTER_THRESHOLD_MS = lattice::config::STALE_MASTER_THRESHOLD_MS;

  // Peer EEPROM management
  void loadPeersFromEEPROM();
  void savePeersToEEPROM();
  void addPeerToEEPROM(const uint8_t mac[6]);
  void removePeerFromEEPROM(const uint8_t mac[6]);

  // Peer logic
  PeerInfo* findPeer(const uint8_t mac[6]);
  bool isPeerInRange(const uint8_t mac[6]);
  PeerInfo* findNextHopToMaster();

  // Bounds-checked insert into peerMacs fixed array
  bool appendPeer(const PeerInfo& peer);

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
  void updatePeerLastSeen(const uint8_t mac[6]);
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

  // Replay protection
  uint32_t bootEpoch;
  uint16_t txSeqNum;

  struct ReplayEntry {
    uint8_t mac[6];
    uint32_t epoch;
    uint16_t seq;
  };
  static constexpr size_t REPLAY_CACHE_SIZE = 16;
  ReplayEntry replayCache[REPLAY_CACHE_SIZE];
  size_t replayCacheIdx;

  bool isReplay(const mesh_message& msg);

  // Beacon relay dedup (fix C10)
  uint32_t lastRelayedEpoch;
  uint16_t lastRelayedSeqNum;

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

  // Beacon timer (moved from broadcastMasterBeacon for loop() integration)
  uint32_t lastBeaconMs;

public:
  Mesh();
  bool init();

  // Static trampoline for Adapter usage
  static void transmit(const adapter_types type, const uint8_t* data);

  void linkDataRecvCallback(std::function<void(mesh_message)> recvCallback);

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
  void addPeer(const uint8_t mac[6]);
  void removePeer(const uint8_t mac[6]);
  const PeerInfo* getPeerList() const { return peerMacs; }
  size_t getPeerCount() const { return peerCount; }

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
