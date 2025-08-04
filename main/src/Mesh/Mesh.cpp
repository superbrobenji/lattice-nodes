#include "Mesh.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"
#include <esp_wifi.h>
#include <cstring>
#include <EEPROM.h>

namespace planetopia {
namespace mesh {

using namespace planetopia::utils;

Mesh* Mesh::instance = nullptr;

// --- Helper: MAC equality ---
static bool macEquals(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 6) == 0;
}

Mesh::Mesh()
  : isMaster(false), lastBeaconMillis(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(currentMaster.nextHop, 0, 6);
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  peerMacs.clear();
}

void Mesh::readMacAddress() {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, deviceMacAddress);
  if (ret != ESP_OK) {
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      ("MESH: Failed to read MAC address: " + String(esp_err_to_name(ret))).c_str());
  } else {
    Logger::log("MESH", "Device MAC: ", LogLevel::LOG_DEBUG);
    printMac(deviceMacAddress);
  }
}

void Mesh::printMac(const uint8_t mac[6]) {
  String macStr;
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) macStr += "0";
    macStr += String(mac[i], HEX);
    if (i < 5) macStr += ":";
  }
  Logger::logln("MESH", macStr, LogLevel::LOG_DEBUG);
}

void Mesh::printMeshMessage(const mesh_message& msg) {
  auto macToStr = [](const uint8_t mac[6]) -> String {
    String s;
    for (int i = 0; i < 6; ++i) {
      if (mac[i] < 0x10) s += "0";
      s += String(mac[i], HEX);
      if (i < 5) s += ":";
    }
    return s;
  };

  Logger::logln("MESH", "------ Mesh Message ------", LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Origin:    " + macToStr(msg.originMacAddress), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Target:    " + macToStr(msg.targetMacAddress), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Last Hop:  " + macToStr(msg.lastHopMacAddress), LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "MsgType:   " + String((uint8_t)msg.messageType) + " (" + (msg.messageType == MESH_TYPE_MASTER_BEACON ? "MASTER_BEACON" : "ADAPTER_DATA") + ")", LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "DataType:  " + String((uint8_t)msg.dataType), LogLevel::LOG_DEBUG);

  String dataStr;
  for (int i = 0; i < 12; ++i) {
    if (msg.data[i] < 0x10) dataStr += "0";
    dataStr += String(msg.data[i], HEX) + " ";
  }
  Logger::logln("MESH", "Data:      " + dataStr, LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "Hop Count: " + String(msg.hopCount), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "-------------------------", LogLevel::LOG_DEBUG);
}

// --- EEPROM Peer Management ---
void Mesh::loadPeersFromEEPROM() {
  peerMacs.clear();
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < MAX_PEERS; ++i) {
    uint8_t mac[6];
    bool allFF = true, all00 = true;
    for (int j = 0; j < 6; ++j) {
      mac[j] = EEPROM.read(EEPROM_PEERLIST_ADDR + i * 6 + j);
      if (mac[j] != 0xFF) allFF = false;
      if (mac[j] != 0x00) all00 = false;
    }
    if (!allFF && !all00) {
      PeerInfo peer;
      memcpy(peer.mac, mac, 6);
      peer.lastSeenMillis = 0;  // will update at runtime
      peerMacs.push_back(peer);
    }
  }
  EEPROM.end();
}

void Mesh::savePeersToEEPROM() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    ErrorHandler::getInstance().signalError(ErrorType::MEMORY_ERROR, "EEPROM.begin failed in savePeersToEEPROM");
    return;
  }
  // Wipe previous list
  for (int i = 0; i < MAX_PEERS * 6; ++i) {
    EEPROM.write(EEPROM_PEERLIST_ADDR + i, 0xFF);
  }
  for (size_t i = 0; i < peerMacs.size() && i < MAX_PEERS; ++i) {
    for (int j = 0; j < 6; ++j) {
      EEPROM.write(EEPROM_PEERLIST_ADDR + i * 6 + j, peerMacs[i].mac[j]);
    }
  }
  EEPROM.commit();
  EEPROM.end();
}

void Mesh::addPeerToEEPROM(const uint8_t mac[6]) {
  if (findPeer(mac) || macEquals(mac, deviceMacAddress)) return;

  if (peerMacs.size() >= MAX_PEERS) {
    ErrorHandler::getInstance().signalError(
      ErrorType::MEMORY_ERROR,
      "Peer list full! Cannot add new peer. MAX_PEERS reached.");
    return;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  peer.lastSeenMillis = millis();
  peerMacs.push_back(peer);
  savePeersToEEPROM();

  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = true;
  memcpy(info.lmk, meshKey, MESH_KEY_SIZE);
  esp_err_t result = esp_now_add_peer(&info);
  if (result != ESP_OK) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      ("Failed to add ESP-NOW peer in addPeerToEEPROM: " + String(esp_err_to_name(result))).c_str());
  } else {
    Logger::logln("MESH", "Peer added for encryption", LogLevel::LOG_DEBUG);
  }
}

void Mesh::removePeerFromEEPROM(const uint8_t mac[6]) {
  for (auto it = peerMacs.begin(); it != peerMacs.end(); ++it) {
    if (macEquals(it->mac, mac)) {
      peerMacs.erase(it);
      break;
    }
  }
  savePeersToEEPROM();
  esp_err_t result = esp_now_del_peer(mac);
  if (result != ESP_OK) {
    ErrorHandler::getInstance().signalError(ErrorType::COMMUNICATION_FAIL, ("Failed to add ESP-NOW peer: " + String(esp_err_to_name(result))).c_str());
  } else {
    Logger::logln("MESH", "Added ESP-NOW peer.", LogLevel::LOG_DEBUG);
  }
}

PeerInfo* Mesh::findPeer(const uint8_t mac[6]) {
  for (auto& peer : peerMacs) {
    if (macEquals(peer.mac, mac)) return &peer;
  }
  return nullptr;
}
bool Mesh::isPeerInRange(const uint8_t mac[6]) {
  PeerInfo* peer = findPeer(mac);
  if (!peer) return false;
  return millis() - peer->lastSeenMillis < 8000;  // Considered in range if seen in last 8s
}
PeerInfo* Mesh::findNextHopToMaster() {
  // For this mesh: nextHop == currentMaster.nextHop
  if (currentMaster.distance == 0xFF) return nullptr;
  for (auto& peer : peerMacs) {
    if (macEquals(peer.mac, currentMaster.nextHop)
        && isPeerInRange(peer.mac)
        && !macEquals(peer.mac, deviceMacAddress))
      return &peer;
  }
  return nullptr;
}

mesh_message Mesh::buildMessage(adapter_types type, const uint8_t data[12], MeshMessageType msgType) {
  mesh_message msg = {};
  msg.messageType = msgType;
  msg.dataType = type;
  memcpy(msg.originMacAddress, deviceMacAddress, 6);
  if (msgType == MESH_TYPE_MASTER_BEACON) {
    memset(msg.targetMacAddress, 0xFF, 6);  // Not used
  } else {
    memcpy(msg.targetMacAddress, currentMaster.mac, 6);
  }
  memcpy(msg.lastHopMacAddress, deviceMacAddress, 6);
  if (data) memcpy(msg.data, data, sizeof(msg.data));
  msg.hopCount = 0;
  return msg;
}

bool Mesh::init() {
  instance = this;
  WiFi.mode(WIFI_STA);

  readMacAddress();

  // Load peer list
  loadPeersFromEEPROM();
  loadMeshKeyFromEEPROM();

  esp_err_t espNowInit = esp_now_init();
  if (espNowInit != ESP_OK) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      ("MESH: Error initializing ESP-NOW: " + String(esp_err_to_name(espNowInit))).c_str());
    return false;
  }

  for (const auto& peer : peerMacs) {
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, peer.mac, 6);
    info.channel = 0;
    info.encrypt = true;
    memcpy(info.lmk, meshKey, MESH_KEY_SIZE);
    esp_err_t result = esp_now_add_peer(&info);
    if (result != ESP_OK) {
      ErrorHandler::getInstance().signalError(ErrorType::COMMUNICATION_FAIL, ("Failed to add ESP-NOW peer: " + String(esp_err_to_name(result))).c_str());
    } else {
      Logger::logln("MESH", "Added ESP-NOW peer.", LogLevel::LOG_DEBUG);
    }
  }
  Logger::logln("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);

  esp_now_register_send_cb(onDataSentCallback);

  esp_now_register_recv_cb(Mesh::dataRecvTrampoline);

  return true;
}

void Mesh::onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status) {
  String statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail";
  Logger::logln("MESH", "Last Packet Send Status: " + statusStr, LogLevel::LOG_DEBUG);
}

void Mesh::onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData, int len) {
  if (!incomingData || len < sizeof(mesh_message)) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      "MESH: Invalid incoming data or length");
    return;
  }

  mesh_message dataToReceive;
  memcpy(&dataToReceive, incomingData, sizeof(dataToReceive));

  Logger::logln("MESH", "Bytes received:", LogLevel::LOG_DEBUG);
  printMeshMessage(dataToReceive);

  // Update lastSeenMillis for sender, add if new
  if (mac && mac->src_addr) {
    if (!macEquals(mac->src_addr, deviceMacAddress)) {
      PeerInfo* p = findPeer(mac->src_addr);
      if (!p) {
        addPeerToEEPROM(mac->src_addr);
      } else {
        p->lastSeenMillis = millis();
      }
    }
  }

  // Multi-master detection
  if (dataToReceive.messageType == MESH_TYPE_MASTER_BEACON) {
    if (!macEquals(lastSeenMasterMac, dataToReceive.originMacAddress) && lastSeenMasterMac[0] != 0) {
      Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
      ErrorHandler::getInstance().signalError(
        ErrorType::CONFIG_ERROR,
        "Multiple master nodes detected! Network split or misconfiguration likely.");
    }
    memcpy(lastSeenMasterMac, dataToReceive.originMacAddress, 6);

    uint8_t beaconHop = dataToReceive.hopCount;
    uint8_t newDistance = beaconHop + 1;

    // Loop prevention
    if (macEquals(dataToReceive.lastHopMacAddress, deviceMacAddress)) {
      Logger::logln("MESH", "Ignoring beacon from myself (loop prevention)", LogLevel::LOG_DEBUG);
      return;
    }

    if (currentMaster.distance == 0xFF || !macEquals(currentMaster.mac, dataToReceive.originMacAddress) || newDistance < currentMaster.distance) {

      memcpy(currentMaster.mac, dataToReceive.originMacAddress, 6);
      currentMaster.distance = newDistance;
      memcpy(currentMaster.nextHop, dataToReceive.lastHopMacAddress, 6);

      Logger::logln("MESH", "Updated route to master. Distance: " + String(newDistance), LogLevel::LOG_INFO);
    }

    // Only relay if not master and not a loop
    if (!isMaster) {
      mesh_message relay = dataToReceive;
      relay.hopCount = newDistance;
      memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
      if (!macEquals(relay.lastHopMacAddress, deviceMacAddress)) {
        transmitCore(relay.dataType, relay.data, MESH_TYPE_MASTER_BEACON, &relay);
        Logger::logln("MESH", "Relayed master beacon. My distance: " + String(newDistance), LogLevel::LOG_DEBUG);
      }
    }
    return;
  }

  // Adapter data messages:
  if (externalRecvCallback) {
    externalRecvCallback(dataToReceive);
  }
}

void Mesh::dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data, int len) {
  if (instance) {
    instance->onDataRecvCallback(mac_addr, data, len);
  }
}

void Mesh::sendMessage(const uint8_t target[6], mesh_message msg) {
  if (macEquals(target, deviceMacAddress)) {
    Logger::logln("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  printMeshMessage(msg);
  esp_err_t result = esp_now_send(target, (uint8_t*)&msg, sizeof(msg));
  if (result == ESP_OK) {
    Logger::logln("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      ("MESH: Error sending message: " + String(esp_err_to_name(result))).c_str());
  }
}

void Mesh::broadcastToAllPeers(mesh_message msg) {
  if (peerMacs.empty()) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (const auto& mac : peerMacs) {
    if (memcmp(mac.mac, deviceMacAddress, 6) == 0) continue;  // Skip self
    memcpy(msg.targetMacAddress, mac.mac, 6);
    sendMessage(mac.mac, msg);
  }
}

void Mesh::transmitCore(const adapter_types type, const uint8_t data[12], MeshMessageType msgType, const mesh_message* msgOverride) {
  mesh_message msg;
  if (msgOverride) {
    msg = *msgOverride;
  } else {
    msg = buildMessage(type, data, msgType);
  }


  // Only for adapter data, set target as master
  if (msgType == MESH_TYPE_ADAPTER_DATA) {
    memcpy(msg.targetMacAddress, currentMaster.mac, 6);
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop = findNextHopToMaster();
  if (nextHop && !macEquals(nextHop->mac, deviceMacAddress)) {
    sendMessage(nextHop->mac, msg);
  } else {
    Logger::logln("MESH", "No next hop to master, cannot send!", LogLevel::LOG_WARN);
    // Optionally, could queue for later
  }
}

void Mesh::transmit(const adapter_types type, const uint8_t data[12]) {
  instance->transmitCore(type, data, MESH_TYPE_ADAPTER_DATA, nullptr);
}

void Mesh::linkDataRecvCallback(std::function<void(mesh_message)> recvCallback) {
  externalRecvCallback = recvCallback;
}

// --- Periodically called in main loop if this node is master ---
void Mesh::broadcastMasterBeacon() {
  unsigned long now = millis();
  if (now - lastBeaconMillis < BEACON_INTERVAL_MS) return;
  lastBeaconMillis = now;

  mesh_message beacon = buildMessage(planetopia::adapter::UNKNOWN_ADAPTER, nullptr, MESH_TYPE_MASTER_BEACON);
  beacon.data[0] = 1;  // protocolVersion, optional
  beacon.hopCount = 0;


  broadcastToAllPeers(beacon);
}

// Optional peer management (can be used in your admin tools)
void Mesh::addPeer(const uint8_t mac[6]) {
  if (!findPeer(mac)) {
    PeerInfo p;
    memcpy(p.mac, mac, 6);
    p.lastSeenMillis = 0;
    peerMacs.push_back(p);
    savePeersToEEPROM();
  }
}
void Mesh::removePeer(const uint8_t mac[6]) {
  removePeerFromEEPROM(mac);
}

void Mesh::loadMeshKeyFromEEPROM() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    ErrorHandler::getInstance().signalError(ErrorType::MEMORY_ERROR, "EEPROM.begin failed in loadMeshKeyFromEEPROM");
    return;
  }
  bool unset = true;
  for (int i = 0; i < MESH_KEY_SIZE; ++i) {
    meshKey[i] = EEPROM.read(EEPROM_KEY_ADDR + i);
    if (meshKey[i] != 0xFF) unset = false;
  }
  EEPROM.end();

  if (unset) {
    ErrorHandler::getInstance().signalError(ErrorType::CONFIG_ERROR, "Mesh encryption key was unset, using default. This is insecure for production!");

    // Key is unset, use default or halt mesh comms
    // Example default, CHANGE THIS FOR PRODUCTION
    const uint8_t defaultKey[MESH_KEY_SIZE] = { 0xBA, 0xAD, 0xF0, 0x0D, 0xBE, 0xEF, 0xC0, 0xDE, 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED, 0xFA, 0xCE };
    memcpy(meshKey, defaultKey, MESH_KEY_SIZE);
    saveMeshKeyToEEPROM(meshKey);
    Logger::logln("MESH", "Mesh key unset, using default and saving to EEPROM!", LogLevel::LOG_WARN);
  }
}

void Mesh::saveMeshKeyToEEPROM(const uint8_t* key) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < MESH_KEY_SIZE; ++i)
    EEPROM.write(EEPROM_KEY_ADDR + i, key[i]);
  EEPROM.commit();
  EEPROM.end();
}

bool Mesh::meshKeyIsSet() const {
  for (int i = 0; i < MESH_KEY_SIZE; ++i)
    if (meshKey[i] != 0xFF) return true;
  return false;
}

}
}
