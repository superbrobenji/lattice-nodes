#ifndef MESH_H
#define MESH_H

#include <functional>
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>
#include <array>
#include <cstdint>
#include "src/Adapter/Adapter.h"
#include "src/persistence/EEPROM_Manager.h"
#include "../../project_config.h"  // Added for global limits/config

namespace planetopia {
namespace mesh {

using planetopia::adapter::adapter_types;
using planetopia::utils::EEPROM_SIZES::MAX_PEERS;  // Use constant from EEPROM_Manager

// --- Mesh protocol message type ---
enum MeshMessageType : uint8_t {
  MESH_TYPE_ADAPTER_DATA = 0,
  MESH_TYPE_MASTER_BEACON = 1,
};

// --- Mesh message struct ---
struct mesh_message {
  MeshMessageType messageType;
  adapter_types dataType;
  uint8_t originMacAddress[6];
  uint8_t targetMacAddress[6];
  uint8_t lastHopMacAddress[6];
  uint8_t data[12];
  uint8_t hopCount;
};

// Peer info struct for RAM and EEPROM storage
struct PeerInfo {
  uint8_t mac[6];
  uint32_t lastSeenMillis;
};

// Master routing info
struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance;    // Hops to master
  uint8_t nextHop[6];  // Next hop MAC
};

class Mesh {
private:
  static constexpr uint32_t BEACON_INTERVAL_MS = planetopia::config::MASTER_BEACON_INTERVAL_MS;
  static constexpr int MESH_KEY_SIZE = 16;

  uint8_t meshKey[MESH_KEY_SIZE];

  static Mesh* instance;

  uint8_t deviceMacAddress[6];
  uint8_t lastSeenMasterMac[6];

  esp_now_peer_info_t peerInfo;

  std::vector<PeerInfo> peerMacs;  // List of all known peers

  void readMacAddress();
  void printMac(const uint8_t mac[6]);
  void printMeshMessage(const mesh_message& msg);

  static void onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status);
  void onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData, int len);
  static void dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data, int len);

  mesh_message buildMessage(adapter_types type, const uint8_t data[12], MeshMessageType msgType);

  std::function<void(mesh_message)> externalRecvCallback;

  MasterInfo currentMaster;
  bool isMaster;
  uint32_t lastBeaconMillis;
  uint32_t lastMasterBeaconReceivedMs;
  static constexpr uint32_t STALE_MASTER_THRESHOLD_MS = planetopia::config::STALE_MASTER_THRESHOLD_MS;

  // Peer EEPROM management
  void loadPeersFromEEPROM();
  void savePeersToEEPROM();
  void addPeerToEEPROM(const uint8_t mac[6]);
  void removePeerFromEEPROM(const uint8_t mac[6]);

  // Peer logic
  PeerInfo* findPeer(const uint8_t mac[6]);
  bool isPeerInRange(const uint8_t mac[6]);
  PeerInfo* findNextHopToMaster();

  void sendMessage(const uint8_t target[6], mesh_message msg);
  void broadcastToAllPeers(mesh_message msg);

  void transmitCore(const adapter_types type, const uint8_t data[12], MeshMessageType msgType = MESH_TYPE_ADAPTER_DATA, const mesh_message* msgOverride = nullptr);

  void loadMeshKeyFromEEPROM();
  void saveMeshKeyToEEPROM(const uint8_t* key);
  void generateRandomMeshKey();
  bool meshKeyIsSet() const;

  // --- Tiger Style refactor helpers ---
  void updatePeerLastSeen(const esp_now_recv_info* info);
  void processMasterBeacon(const mesh_message& msg);
  void processAdapterData(const mesh_message& msg);

  // Setup helpers (Tiger Style refactor)
  bool setupWiFi();
  bool setupEspNow();
  void loadPersistentState();

  // Curve25519 keypair
  uint8_t devicePrivateKey[32];
  uint8_t devicePublicKey[32];
  void loadOrGenerateKeypair();

public:
  Mesh();
  bool init();

  // Static trampoline for Adapter usage
  static void transmit(const adapter_types type, const uint8_t data[12]);

  void linkDataRecvCallback(std::function<void(mesh_message)> recvCallback);

  // Master beacon: call in main loop if node is master; handles timing internally
  void broadcastMasterBeacon();

  // Master timeout check: call in main loop; clears stale master route on timeout
  void checkMasterTimeout();

  // Node role config
  void setIsMaster(bool value) {
    isMaster = value;
  }
  bool getIsMaster() const {
    return isMaster;
  }

  // Peer management API (optional, can be used in your app/UI)
  void addPeer(const uint8_t mac[6]);
  void removePeer(const uint8_t mac[6]);
  const std::vector<PeerInfo>& getPeerList() const {
    return peerMacs;
  }

  // Broadcast adapter data to all peers
  void broadcastAdapterData(adapter_types type, const uint8_t data[12]);

  // Serial adapter helper (optional broadcast)
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t data[12]);

  // Debug helper
  void debugDumpRadio();

  // Provisioning: public key accessor (private key never exposed)
  const uint8_t* getDevicePublicKey() const { return devicePublicKey; }
};

}
}

#endif  // MESH_H
