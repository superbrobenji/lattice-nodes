#include "Mesh.h"
#include "src/network/MacAddress.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"  // unified error
#include "src/persistence/EEPROM_Manager.h"
// Error.h already provides ERROR_CHECK macros
#include <esp_now.h>
#include <WiFi.h>
#include <cstring>
#include "../../project_config.h"

namespace planetopia {
namespace mesh {

using namespace planetopia::utils;

Mesh* Mesh::instance = nullptr;

// no longer need macEquals helper – use MacAddress equality directly

static void registerPeerWithEspNow(const uint8_t mac[6], const uint8_t* lmk) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = true;
  memcpy(info.lmk, lmk, 16);
  planetopia::err::checkEsp(esp_now_add_peer(&info), planetopia::utils::ErrorType::COMMUNICATION_FAIL, "registerPeerWithEspNow: add_peer failed");
}

Mesh::Mesh()
  : isMaster(false), lastBeaconMillis(0), lastMasterBeaconReceivedMs(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(currentMaster.nextHop, 0, 6);
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  peerMacs.clear();
  peerMacs.reserve(MAX_PEERS);  // pre-allocate to avoid runtime realloc
}

void Mesh::readMacAddress() {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, deviceMacAddress);
  if (ret != ESP_OK) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                         planetopia::core::ModuleDigit::MESH,
                         1,
                         (String("MESH: Failed to read MAC address: ") + esp_err_to_name(ret)).c_str());
  } else {
    Logger::log("MESH", "Device MAC: ", LogLevel::LOG_DEBUG);
    Logger::logln("MESH", planetopia::utils::MacAddress(deviceMacAddress).toString(), LogLevel::LOG_DEBUG);
  }
}

void Mesh::printMeshMessage(const mesh_message& msg) {
  auto macToStr = [](const uint8_t mac[6]) { return planetopia::utils::MacAddress(mac).toString(); };

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

  uint8_t peerList[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_MAC_SIZE];
  bool eepromOk = EEPROM_Manager::getInstance().loadPeerList(peerList, EEPROM_SIZES::MAX_PEERS);

  if (eepromOk) {
    for (int i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
      bool valid = true;
      uint8_t mac[6];
      for (int j = 0; j < 6; ++j) {
        mac[j] = peerList[i * 6 + j];
        if (mac[j] == 0xFF) valid = false;  // treat 0xFF as empty
      }
      if (valid) {
        PeerInfo peer;
        memcpy(peer.mac, mac, 6);
        peer.lastSeenMillis = 0;
        peerMacs.push_back(peer);
      }
    }
  }

  // Fallback in dev mode or when list is empty
  if (peerMacs.empty()) {
    Logger::logln("MESH", "Peer list empty; loading defaults from config", LogLevel::LOG_INFO);
    for (int i = 0; i < planetopia::config::NUM_DEFAULT_PEERS; ++i) {
      PeerInfo peer;
      memcpy(peer.mac, planetopia::config::DEFAULT_PEERS[i], 6);
      peer.lastSeenMillis = 0;
      peerMacs.push_back(peer);
    }
  }
}

void Mesh::savePeersToEEPROM() {
  // Convert peer list to flat array for EEPROM_Manager
  uint8_t peerList[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_MAC_SIZE];
  memset(peerList, 0xFF, sizeof(peerList));  // Initialize with 0xFF

  for (size_t i = 0; i < peerMacs.size() && i < EEPROM_SIZES::MAX_PEERS; ++i) {
    for (int j = 0; j < 6; ++j) {
      peerList[i * 6 + j] = peerMacs[i].mac[j];
    }
  }

  EEPROM_Manager::getInstance().savePeerList(peerList, peerMacs.size());
}

void Mesh::addPeerToEEPROM(const uint8_t mac[6]) {
  if (findPeer(mac) || planetopia::utils::MacAddress(mac) == planetopia::utils::MacAddress(deviceMacAddress)) return;

  if (peerMacs.size() >= MAX_PEERS) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                         planetopia::core::ModuleDigit::MESH,
                         2,
                         "Peer list full! Cannot add new peer. MAX_PEERS reached.");
    return;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  peer.lastSeenMillis = millis();
  peerMacs.push_back(peer);
  savePeersToEEPROM();
  registerPeerWithEspNow(peer.mac, meshKey);  // This is the only call needed
  Logger::logln("MESH", "Peer added", LogLevel::LOG_DEBUG);
}

void Mesh::removePeerFromEEPROM(const uint8_t mac[6]) {
  for (auto it = peerMacs.begin(); it != peerMacs.end(); ++it) {
    if (planetopia::utils::MacAddress(it->mac) == planetopia::utils::MacAddress(mac)) {
      peerMacs.erase(it);
      break;
    }
  }
  savePeersToEEPROM();
  esp_err_t result = esp_now_del_peer(mac);
  planetopia::err::checkEsp(result, planetopia::utils::ErrorType::COMMUNICATION_FAIL, "removePeerFromEEPROM: del_peer failed");
  Logger::logln("MESH", "Removed ESP-NOW peer.", LogLevel::LOG_DEBUG);
}

PeerInfo* Mesh::findPeer(const uint8_t mac[6]) {
  for (auto& peer : peerMacs) {
    if (planetopia::utils::MacAddress(peer.mac) == planetopia::utils::MacAddress(mac)) return &peer;
  }
  return nullptr;
}
bool Mesh::isPeerInRange(const uint8_t mac[6]) {
  PeerInfo* peer = findPeer(mac);
  if (!peer) return false;
  return millis() - peer->lastSeenMillis < planetopia::config::STALE_PEER_THRESHOLD_MS;
}
PeerInfo* Mesh::findNextHopToMaster() {
  // For this mesh: nextHop == currentMaster.nextHop
  if (currentMaster.distance == 0xFF) return nullptr;
  for (auto& peer : peerMacs) {
    if (planetopia::utils::MacAddress(peer.mac) == planetopia::utils::MacAddress(currentMaster.nextHop)
        && isPeerInRange(peer.mac)
        && planetopia::utils::MacAddress(peer.mac) != planetopia::utils::MacAddress(deviceMacAddress))
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

// ---------- Tiger Style init helpers ----------
bool Mesh::init() {
  instance = this;

  // 1. Load persisted peers/keys
  loadPersistentState();

  // 2. Configure Wi-Fi
  if (!setupWiFi()) return false;

  // 3. Init ESP-NOW
  if (!setupEspNow()) return false;

  return true;
}

bool Mesh::setupWiFi() {
  WiFi.mode(WIFI_STA);
  planetopia::err::checkEsp(
    esp_wifi_set_channel(planetopia::config::WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE),
    planetopia::utils::ErrorType::HARDWARE_FAILURE,
    "Failed to set WiFi channel");

  readMacAddress();
  return true;
}

void Mesh::loadPersistentState() {
  loadPeersFromEEPROM();
  loadMeshKeyFromEEPROM();
}

bool Mesh::setupEspNow() {
  esp_err_t res = esp_now_init();
  if (res != ESP_OK) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::MESH,
                         3,
                         (String("MESH: esp_now_init failed: ") + esp_err_to_name(res)).c_str());
    return false;
  }
  esp_now_set_pmk(meshKey);
  for (auto& p : peerMacs) {
    registerPeerWithEspNow(p.mac, meshKey);
  }
  esp_now_register_send_cb(onDataSentCallback);
  esp_now_register_recv_cb(Mesh::dataRecvTrampoline);
  Logger::logln("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);
  return true;
}
// ------------------------------------------------


void Mesh::onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status) {
  String statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail";
  Logger::logln("MESH", "Last Packet Send Status: " + statusStr, LogLevel::LOG_DEBUG);
}

void Mesh::onDataRecvCallback(const esp_now_recv_info* info, const uint8_t* incomingData, int len) {
  planetopia::err::check(incomingData != nullptr, planetopia::utils::ErrorType::CONFIG_ERROR, "Mesh incomingData is null");
  planetopia::err::check(len >= sizeof(mesh_message), planetopia::utils::ErrorType::CONFIG_ERROR, "Mesh data length too small");
  if (!incomingData || len < sizeof(mesh_message)) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::MESH,
                         4,
                         "MESH: Invalid incoming data or length");
    return;
  }

  mesh_message msg;
  memcpy(&msg, incomingData, sizeof(msg));

  Logger::logln("MESH", "Bytes received:", LogLevel::LOG_DEBUG);
  printMeshMessage(msg);

  // Update peer last-seen
  updatePeerLastSeen(info);

  switch (msg.messageType) {
    case MESH_TYPE_MASTER_BEACON:
      processMasterBeacon(msg);
      break;
    case MESH_TYPE_ADAPTER_DATA:
      processAdapterData(msg);
      break;
    default:
      Logger::logln("MESH", "Unknown message type", LogLevel::LOG_WARN);
      break;
  }
}

void Mesh::dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data, int len) {
  if (instance) {
    planetopia::utils::ErrorCore::getInstance().setCallbackContext(true);
    instance->onDataRecvCallback(mac_addr, data, len);
    planetopia::utils::ErrorCore::getInstance().setCallbackContext(false);
  }
}

void Mesh::sendMessage(const uint8_t target[6], mesh_message msg) {
  if (planetopia::utils::MacAddress(target) == planetopia::utils::MacAddress(deviceMacAddress)) {
    Logger::logln("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  printMeshMessage(msg);
  esp_err_t result = esp_now_send(target, (uint8_t*)&msg, sizeof(msg));
  if (result == ESP_OK) {
    Logger::logln("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::MESH,
                         5,
                         (String("MESH: Error sending message: ") + esp_err_to_name(result)).c_str());
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
  if (nextHop && planetopia::utils::MacAddress(nextHop->mac) != planetopia::utils::MacAddress(deviceMacAddress)) {
    sendMessage(nextHop->mac, msg);
  } else {
    Logger::logln("MESH", "No next hop to master — message dropped. Master timeout or unreachable.", LogLevel::LOG_WARN);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::MESH, 8,
                         "MESH: message dropped, no route to master");
  }
}

void Mesh::transmit(const adapter_types type, const uint8_t data[12]) {
  if (!instance) {
    Logger::logln("MESH", "transmit() called before init", LogLevel::LOG_WARN);
    return;
  }
  if (instance->isMaster) {
    instance->broadcastAdapterData(type, data);
    return;
  }
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
  beacon.data[0] = 1;  // protocolVersion
  beacon.hopCount = 0;

  // Always send a broadcast frame so new nodes can discover the master even
  // when they are not yet in the peer list.
  esp_err_t br = esp_now_send(nullptr, (uint8_t*)&beacon, sizeof(beacon));
  Logger::logln("MESH", String("Beacon broadcast ") + (br == ESP_OK ? "OK" : "FAIL"), LogLevel::LOG_DEBUG);

  // Then unicast to known peers for reliability
  broadcastToAllPeers(beacon);
}

// Optional peer management (can be used in your admin tools)
void Mesh::addPeer(const uint8_t mac[6]) {
  if (!findPeer(mac)) {
    if (peerMacs.size() >= MAX_PEERS) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                            planetopia::core::ModuleDigit::MESH,
                            6,
                            "Peer list full! Cannot add new peer. MAX_PEERS reached.");
      Logger::logln("MESH", "Peer list is full, skipping add", LogLevel::LOG_WARN);
      return;
    }
    PeerInfo p;
    memcpy(p.mac, mac, 6);
    p.lastSeenMillis = 0;
    peerMacs.push_back(p);
    savePeersToEEPROM();
    registerPeerWithEspNow(p.mac, meshKey);
  }
}
void Mesh::removePeer(const uint8_t mac[6]) {
  removePeerFromEEPROM(mac);
}

void Mesh::loadMeshKeyFromEEPROM() {
  // Attempt to load mesh key from EEPROM
  if (!EEPROM_Manager::getInstance().loadMeshKey(meshKey, MESH_KEY_SIZE)) {
    Logger::logln("MESH", "EEPROM read failed, using default mesh key", LogLevel::LOG_WARN);
  }

  // If in DEV_MODE always override with compile-time key
  if (planetopia::config::DEV_MODE) {
    memcpy(meshKey, planetopia::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    Logger::logln("MESH", "DEV_MODE: Overriding mesh key with compile-time default", LogLevel::LOG_INFO);
  }

  // Check if key is unset (all 0xFF or all 0x00)
  bool unset = true;
  for (int i = 0; i < MESH_KEY_SIZE; ++i) {
    if (meshKey[i] != 0xFF && meshKey[i] != 0x00) {
      unset = false;
      break;
    }
  }

  if (unset) {
    Logger::logln("MESH", "Mesh key unset, loading default from config", LogLevel::LOG_INFO);
    memcpy(meshKey, planetopia::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    saveMeshKeyToEEPROM(meshKey);  // Will be skipped automatically in dev mode
  }
}

void Mesh::saveMeshKeyToEEPROM(const uint8_t* key) {
  EEPROM_Manager::getInstance().saveMeshKey(key, MESH_KEY_SIZE);
}

// Generate a new random 16-byte mesh key
void Mesh::generateRandomMeshKey() {
  for (int i = 0; i < MESH_KEY_SIZE; ++i) {
    meshKey[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  }
  Logger::logln("MESH", "Generated new random mesh key", LogLevel::LOG_DEBUG);
}

bool Mesh::meshKeyIsSet() const {
  for (int i = 0; i < MESH_KEY_SIZE; ++i)
    if (meshKey[i] != 0xFF) return true;
  return false;
}

void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[12]) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  // Broadcast: set target to FF:FF:... and hopCount=0
  memset(msg.targetMacAddress, 0xFF, 6);
  msg.hopCount = 0;
  broadcastToAllPeers(msg);
}

void Mesh::broadcastAdapterDataStatic(adapter_types type, const uint8_t data[12]) {
  if (instance) instance->broadcastAdapterData(type, data);
}

void Mesh::debugDumpRadio() {
  uint8_t ch;
  esp_wifi_get_channel(&ch, nullptr);
  String out = String("DBG Channel=") + String(ch) + " Key=";
  for (int i = 0; i < MESH_KEY_SIZE; ++i) {
    if (meshKey[i] < 0x10) out += "0";
    out += String(meshKey[i], HEX) + " ";
  }
  Logger::logln("MESH", out, LogLevel::LOG_INFO);
}

void Mesh::checkMasterTimeout() {
  if (isMaster) return;
  if (currentMaster.distance == 0xFF) return;  // No master known yet
  if (millis() - lastMasterBeaconReceivedMs > MASTER_TIMEOUT_MS) {
    Logger::logln("MESH", "Master beacon timeout — clearing route, treating as offline", LogLevel::LOG_WARN);
    memset(currentMaster.mac, 0, 6);
    currentMaster.distance = 0xFF;
    memset(currentMaster.nextHop, 0, 6);
    memset(lastSeenMasterMac, 0, 6);
    lastMasterBeaconReceivedMs = 0;
  }
}

// ---------- Tiger Style helper implementations ----------
void Mesh::updatePeerLastSeen(const esp_now_recv_info* info) {
  if (!info || !info->src_addr) return;
  if (planetopia::utils::MacAddress(info->src_addr) == planetopia::utils::MacAddress(deviceMacAddress)) return;
  PeerInfo* p = findPeer(info->src_addr);
  if (!p) {
    addPeerToEEPROM(info->src_addr);
  } else {
    p->lastSeenMillis = millis();
  }
}

void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hopCount >= planetopia::config::MAX_HOPS) {
    Logger::logln("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  if (planetopia::utils::MacAddress(lastSeenMasterMac) != planetopia::utils::MacAddress(msg.originMacAddress) && lastSeenMasterMac[0] != 0) {
    Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                         planetopia::core::ModuleDigit::MESH,
                         7,
                         "Multiple master nodes detected! Network split or misconfiguration likely.");
  }
  memcpy(lastSeenMasterMac, msg.originMacAddress, 6);
  lastMasterBeaconReceivedMs = millis();

  uint8_t newDistance = msg.hopCount + 1;
  if (currentMaster.distance == 0xFF || planetopia::utils::MacAddress(currentMaster.mac) != planetopia::utils::MacAddress(msg.originMacAddress) || newDistance < currentMaster.distance) {
    memcpy(currentMaster.mac, msg.originMacAddress, 6);
    currentMaster.distance = newDistance;
    memcpy(currentMaster.nextHop, msg.lastHopMacAddress, 6);
    Logger::logln("MESH", "Updated route to master. Distance: " + String(newDistance), LogLevel::LOG_INFO);
  }

  if (!isMaster) {
    mesh_message relay = msg;
    relay.hopCount = newDistance;
    memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
    transmitCore(relay.dataType, relay.data, MESH_TYPE_MASTER_BEACON, &relay);
  }
}

void Mesh::processAdapterData(const mesh_message& msg) {
  if (externalRecvCallback) externalRecvCallback(msg);
}
// --------------------------------------------------------

}
}
