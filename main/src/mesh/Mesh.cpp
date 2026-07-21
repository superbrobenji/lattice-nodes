#include "Mesh.h"
#include "src/network/MacAddress.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h" // unified error
#include "src/persistence/EepromManager.h"
// Error.h already provides ERROR_CHECK macros
#include <esp_now.h>
#include <WiFi.h>
#include <cstring>
#include "../../project_config.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "MeshCrypto.h"
#include "E2ECrypto.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

Mesh* Mesh::instance = nullptr;

// no longer need macEquals helper – use MacAddress equality directly

Mesh::Mesh()
    : isMaster(false), lastBeaconMillis(0), lastMasterBeaconReceivedMs(0),
      _dualMasterMode(lattice::config::DUAL_MASTER_MODE), recvQueueHead(0), recvQueueTail(0),
      lastBeaconMs(0), lastRouteReportMs(0), relayPending(false), relayPendingAt(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(currentMaster.nextHop, 0, 6);
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  memset(recvQueue, 0, sizeof(recvQueue));
  memset(&relayPendingMsg, 0, sizeof(relayPendingMsg));
}

void Mesh::readMacAddress() {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, deviceMacAddress);
  if (ret != ESP_OK) {
    lattice::err::fail(
        lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::MESH, 1,
        (String("MESH: Failed to read MAC address: ") + esp_err_to_name(ret)).c_str());
  } else {
    Logger::log("MESH", "Device MAC: ", LogLevel::LOG_DEBUG);
    Logger::logln("MESH", lattice::utils::MacAddress(deviceMacAddress).toString(),
                  LogLevel::LOG_DEBUG);
  }
}

void Mesh::printMeshMessage(const mesh_message& msg) {
  auto macToStr = [](const uint8_t(&mac)[6]) { return lattice::utils::MacAddress(mac).toString(); };

  Logger::logln("MESH", "------ Mesh Message ------", LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Origin:    " + macToStr(msg.origin_mac_address), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Target:    " + macToStr(msg.target_mac_address), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "Last Hop:  " + macToStr(msg.last_hop_mac_address), LogLevel::LOG_DEBUG);

  Logger::logln(
      "MESH",
      "MsgType:   " + String((uint8_t)msg.message_type) + " (" +
          (msg.message_type == MESH_TYPE_MASTER_BEACON ? "MASTER_BEACON" : "ADAPTER_DATA") + ")",
      LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "DataType:  " + String((uint8_t)msg.data_type), LogLevel::LOG_DEBUG);

  String dataStr;
  for (int i = 0; i < 12; ++i) {
    if (msg.data[i] < 0x10)
      dataStr += "0";
    dataStr += String(msg.data[i], HEX) + " ";
  }
  Logger::logln("MESH", "Data:      " + dataStr, LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "Hop Count: " + String(msg.hop_count), LogLevel::LOG_DEBUG);
  Logger::logln("MESH", "-------------------------", LogLevel::LOG_DEBUG);
}

PeerInfo* Mesh::findNextHopToMaster() {
  if (currentMaster.distance == 0xFF)
    return nullptr;

  // Prefer an enrolled peer that is the direct master and in range (distance 1,
  // the common single-hop case) — keeps the existing behavior and E2E peering.
  PeerInfo* direct = peers.find(currentMaster.mac);
  if (direct && currentMaster.distance == 1 && peers.isPeerInRange(direct->mac) &&
      lattice::utils::MacAddress(direct->mac) != lattice::utils::MacAddress(deviceMacAddress))
    return direct;

  // Multi-hop (spec §3): pick the freshest neighbor strictly closer to the
  // master from the NeighborTable. The relay need not be an enrolled peer.
  uint8_t hopMac[6];
  if (!neighbors.selectNextHop(currentMaster.distance, millis(), hopMac))
    return nullptr;
  if (lattice::utils::MacAddress(hopMac) == lattice::utils::MacAddress(deviceMacAddress))
    return nullptr;

  // Bound the auto-registered forwarding peer to exactly one (spec §2:
  // "20-peer cap, LRU-evicted"). A node forwards uplink to only one next hop
  // at a time, so if we previously auto-registered a DIFFERENT relay, evict
  // it before registering the new one — otherwise a beacon-flooding attacker
  // spoofing distinct relay MACs could exhaust the ~20-slot ESP-NOW peer
  // table (no self-heal, no reboot) and blackhole the real uplink. Never
  // evict an enrolled peer (master or sensor) — those live in `peers` and
  // are managed exclusively by the enrollment path.
  static const uint8_t kZeroMac[6] = {0, 0, 0, 0, 0, 0};
  bool isNewRelay =
      lattice::utils::MacAddress(forwardingPeer) != lattice::utils::MacAddress(hopMac);
  bool forwardingPeerSet = memcmp(forwardingPeer, kZeroMac, 6) != 0;
  if (forwardingPeerSet && isNewRelay && !peers.find(forwardingPeer) &&
      lattice::utils::MacAddress(forwardingPeer) != lattice::utils::MacAddress(currentMaster.mac)) {
    if (esp_now_is_peer_exist(forwardingPeer))
      esp_now_del_peer(forwardingPeer);
  }

  // Auto-register the chosen next hop as an unencrypted ESP-NOW peer (spec §3).
  // Idempotent — registerPeerWithEspNow no-ops if the peer already exists.
  lattice::mesh::crypto::registerPeerWithEspNow(hopMac);
  memcpy(forwardingPeer, hopMac, 6);

  memcpy(nextHopScratch.mac, hopMac, 6);
  return &nextHopScratch;
}

uint16_t Mesh::nextSeqGuarded() {
  uint16_t seq = replay.nextSeq();
  if (seq == 0) {
    // seq wrapped (spec §2): a reused (epoch, seq) pair would reuse an AEAD nonce.
    // Advance the persisted epoch and restart the sequence. replay.bootEpoch is
    // already the currently active epoch (kept in sync with EEPROM by init()
    // and by this method), so bump from it directly rather than re-reading
    // EEPROM.
    uint32_t epoch = replay.bootEpoch + 1;
    EepromManager::getInstance().saveBootEpoch(epoch);
    replay.bootEpoch = epoch;
    seq = replay.nextSeq();
  }
  return seq;
}

mesh_message Mesh::buildMessage(adapter_types type, const uint8_t* data, MeshMessageType msgType) {
  mesh_message msg = {};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = msgType;
  msg.data_type = type;
  memcpy(msg.origin_mac_address, deviceMacAddress, 6);
  if (msgType == MESH_TYPE_MASTER_BEACON) {
    memset(msg.target_mac_address, 0xFF, 6); // Not used
  } else {
    memcpy(msg.target_mac_address, currentMaster.mac, 6);
  }
  memcpy(msg.last_hop_mac_address, deviceMacAddress, 6);
  if (data)
    memcpy(msg.data, data, sizeof(msg.data));
  msg.hop_count = 0;
  msg.seq_num = nextSeqGuarded();
  msg.epoch_num = replay.bootEpoch;
  return msg;
}

// ---------- Tiger Style init helpers ----------
bool Mesh::init() {
  // instance already set in constructor; no need to repeat
  // 1. Load persisted peers/keys
  loadPersistentState();

  // 2. Increment and save boot epoch (replay protection)
  uint32_t epoch = EepromManager::getInstance().loadBootEpoch() + 1;
  EepromManager::getInstance().saveBootEpoch(epoch);
  replay.init(epoch);
  Logger::logln("MESH", "Boot epoch: " + String(replay.bootEpoch), LogLevel::LOG_INFO);

  // 3. Configure Wi-Fi
  if (!setupWiFi())
    return false;

  // 3a. Apply TX power preset from EEPROM (deployment-specific)
  {
    lattice::config::TxPowerPreset preset = EepromManager::getInstance().loadTxPowerPreset();
    uint8_t txPowerVal = lattice::config::TX_POWER_VALUES[static_cast<uint8_t>(preset)];
    esp_err_t txErr = esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerVal));
    if (txErr != ESP_OK) {
      Logger::logln("MESH", String("TX power set failed: ") + esp_err_to_name(txErr),
                    LogLevel::LOG_WARN);
    } else {
      Logger::logln("MESH", "TX power preset applied", LogLevel::LOG_INFO);
    }
  }

  // 4. Init ESP-NOW
  if (!setupEspNow())
    return false;

  return true;
}

bool Mesh::setupWiFi() {
  if (!WiFi.mode(WIFI_STA)) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 6,
                       "MESH: Failed to set WiFi mode STA");
    return false;
  }
  lattice::err::checkEsp(esp_wifi_set_channel(lattice::config::WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE),
                         lattice::utils::ErrorType::HARDWARE_FAILURE, "Failed to set WiFi channel");

  readMacAddress();
  peers.setDeviceMac(deviceMacAddress);
  return true;
}

void Mesh::loadPersistentState() {
  peers.loadFromEEPROM();
  loadMeshKeyFromEEPROM();
  enrollment.init();
  if (enrollment.hasMasterMac) {
    Logger::logln("MESH", "Known master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
  if (_dualMasterMode && enrollment.hasMasterMacSecondary) {
    Logger::logln("MESH", "Known secondary master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
}

bool Mesh::setupEspNow() {
  esp_err_t res = esp_now_init();
  if (res != ESP_OK) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 3,
                       (String("MESH: esp_now_init failed: ") + esp_err_to_name(res)).c_str());
    return false;
  }
  lattice::err::checkEsp(esp_now_set_pmk(meshKey), lattice::utils::ErrorType::HARDWARE_FAILURE,
                         "Failed to set ESP-NOW PMK");

  // Register the broadcast MAC so esp_now_send(broadcastMac, ...) reaches all
  // nodes — including unregistered ones. esp_now_send(nullptr, ...) only delivers
  // to already-registered peers; using the explicit FF:FF:… MAC is required for a
  // true 802.11 broadcast frame.
  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (!esp_now_is_peer_exist(broadcastMac)) {
    esp_now_peer_info_t broadcast = {};
    memset(broadcast.peer_addr, 0xFF, 6);
    broadcast.channel = 0;
    broadcast.encrypt = false;
    esp_now_add_peer(&broadcast);
  }

  for (size_t i = 0; i < peers.peerCount; ++i) {
    lattice::mesh::crypto::registerPeerWithEspNow(peers.peerMacs[i].mac);
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

void IRAM_ATTR Mesh::onDataRecvCallback(const esp_now_recv_info* info, const uint8_t* incomingData,
                                        int len) {
  if (!instance || !info || !incomingData)
    return;
  if (static_cast<size_t>(len) < sizeof(mesh_message))
    return;

  uint8_t nextHead = (instance->recvQueueHead + 1) % RECV_QUEUE_SIZE;
  if (nextHead == instance->recvQueueTail) {
    // Queue full — drop packet
    return;
  }

  RecvQueueEntry& slot = instance->recvQueue[instance->recvQueueHead];
  memcpy(slot.srcMac, info->src_addr, 6);
  memcpy(&slot.msg, incomingData, sizeof(mesh_message));
  instance->recvQueueHead = nextHead;
}

void Mesh::drainRecvQueue() {
  while (recvQueueTail != recvQueueHead) {
    RecvQueueEntry& entry = recvQueue[recvQueueTail];
    recvQueueTail = (recvQueueTail + 1) % RECV_QUEUE_SIZE;

    const mesh_message& msg = entry.msg;

    // Proto version check: drop anything that isn't exactly the current wire
    // version. There is no legitimate proto_version==0 case — buildMessage(),
    // Enrollment::sendRequest(), and the JOIN_ACK path all stamp PROTO_VERSION
    // unconditionally — so a zero value only ever means a forged/malformed
    // frame that would otherwise bypass both this flag-day drop and the replay
    // gate below (which is itself keyed on proto_version == PROTO_VERSION).
    if (msg.proto_version != PROTO_VERSION) {
      Logger::logln("MESH", "Unsupported proto version, dropping", LogLevel::LOG_WARN);
      continue;
    }

    // Replay check
    if (msg.proto_version == PROTO_VERSION && msg.epoch_num > 0) {
      if (replay.isReplay(msg)) {
        Logger::logln("MESH", "Replayed message dropped", LogLevel::LOG_DEBUG);
        continue;
      }
    }

    // Update last-seen for known peers only (no EEPROM write — see Task 4)
    peers.updateLastSeen(entry.srcMac);

    switch (msg.message_type) {
    case MESH_TYPE_ENROLLMENT:
      if (isMaster)
        enrollment.processRequest(msg);
      else
        relayEnrollmentUplink(msg);
      break;
    case MESH_TYPE_JOIN_ACK:
      processJoinAck(msg);
      break;
    case MESH_TYPE_MASTER_BEACON:
      processMasterBeacon(msg);
      break;
    case MESH_TYPE_ADAPTER_DATA:
      processAdapterData(msg);
      break;
    case MESH_TYPE_ROUTE_REPORT:
      processRouteReport(msg);
      break;
    default:
      Logger::logln("MESH", "Unknown message type, dropping", LogLevel::LOG_WARN);
    }
  }
}

void IRAM_ATTR Mesh::dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data,
                                        int len) {
  if (!instance)
    return;
  instance->onDataRecvCallback(mac_addr, data, len);
}

void Mesh::sendMessage(const uint8_t* target, const mesh_message& msg) {
  if (lattice::utils::MacAddress(target) == lattice::utils::MacAddress(deviceMacAddress)) {
    Logger::logln("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  esp_err_t result = esp_now_send(target, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (result == ESP_OK) {
    Logger::logln("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 5,
                       (String("MESH: Error sending message: ") + esp_err_to_name(result)).c_str());
  }
}

void Mesh::broadcastToAllPeers(const mesh_message& msg) {
  if (peers.peerCount == 0) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peers.peerCount; ++i) {
    if (memcmp(peers.peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue; // Skip self
    sendMessage(peers.peerMacs[i].mac, msg);
  }
}

bool Mesh::isSealedType(uint8_t messageType) {
  return messageType == MESH_TYPE_ADAPTER_DATA || messageType == MESH_TYPE_ROUTE_REPORT;
}

bool Mesh::masterE2EKeys(const uint8_t** kUp, const uint8_t** kDown) {
  if (!enrollment.hasMasterMac)
    return false;
  PeerInfo* master = peers.find(currentMaster.mac);
  if (!master)
    return false;
  return e2eKeys.getKeys(master->mac, enrollment.getPrivateKey(), master->publicKey, kUp, kDown);
}

bool Mesh::peerE2EKeys(const uint8_t* originMac, const uint8_t** kUp, const uint8_t** kDown) {
  PeerInfo* peer = peers.find(originMac);
  if (!peer)
    return false;
  return e2eKeys.getKeys(peer->mac, enrollment.getPrivateKey(), peer->publicKey, kUp, kDown);
}

void Mesh::transmitCore(const adapter_types type, const uint8_t* data, MeshMessageType msgType,
                        const mesh_message* msgOverride) {
  mesh_message msg;
  if (msgOverride) {
    msg = *msgOverride;
  } else {
    msg = buildMessage(type, data, msgType);
  }

  // Only for adapter data, set target as master
  if (msgType == MESH_TYPE_ADAPTER_DATA) {
    memcpy(msg.target_mac_address, currentMaster.mac,
           6); // authoritative: overrides relay's original target
  }

  // E2E seal (spec §1/§2): self-originated uplink payloads only. Relayed frames
  // (msgOverride with foreign origin) are already sealed — forward untouched.
  bool selfOriginated = (memcmp(msg.origin_mac_address, deviceMacAddress, 6) == 0);
  if (!isMaster && selfOriginated && isSealedType(msg.message_type)) {
    const uint8_t *kUp, *kDown;
    if (!masterE2EKeys(&kUp, &kDown) || !lattice::mesh::crypto::sealPayload(kUp, msg)) {
      Logger::logln("MESH", "E2E seal unavailable — uplink dropped", LogLevel::LOG_WARN);
      return;
    }
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop = findNextHopToMaster();
  if (nextHop &&
      lattice::utils::MacAddress(nextHop->mac) != lattice::utils::MacAddress(deviceMacAddress)) {
    sendMessage(nextHop->mac, msg);
  } else {
    // No route to master is a routine, self-healing transient: a node that has
    // just booted (or whose master went stale) legitimately has no next hop
    // until it hears the next beacon. Drop the frame quietly rather than
    // escalating to err::fail — escalation here drives the error LED and
    // reboot-reason tracking, and turns every such gap (see
    // docs/design-gaps/multihop-data-uplink.md) into an error loop instead of a
    // silent drop. The upstream sender retries on its own timer.
    Logger::logln("MESH", "No next hop to master — message dropped. Master timeout or unreachable.",
                  LogLevel::LOG_WARN);
  }
}

void Mesh::transmitDispatch(const adapter_types type, const uint8_t* data, bool selfOriginated) {
  if (isMaster) {
    broadcastAdapterData(type, data, selfOriginated);
    return;
  }
  transmitCore(type, data, MESH_TYPE_ADAPTER_DATA, nullptr);
}

void Mesh::transmit(const adapter_types type, const uint8_t* data) {
  if (!instance) {
    Logger::logln("MESH", "transmit() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->transmitDispatch(type, data, false);
}

void Mesh::transmitSelfOriginated(const adapter_types type, const uint8_t* data) {
  if (!instance) {
    Logger::logln("MESH", "transmitSelfOriginated() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->transmitDispatch(type, data, true);
}

void Mesh::linkDataRecvCallback(std::function<void(const mesh_message&)> recvCallback) {
  externalRecvCallback = recvCallback;
}

// --- Periodically called in main loop if this node is master ---
void Mesh::broadcastMasterBeacon() {
  unsigned long now = millis();
  if (now - lastBeaconMillis < lattice::config::MASTER_BEACON_INTERVAL_MS)
    return;
  lastBeaconMillis = now;

  mesh_message beacon =
      buildMessage(adapter_types::UNKNOWN_ADAPTER, nullptr, MESH_TYPE_MASTER_BEACON);
  beacon.data[0] = 1; // protocolVersion
  beacon.hop_count = 0;

  // Broadcast-only: send to the registered FF:FF:… broadcast peer so the frame
  // reaches all nodes — including those not yet individually registered.
  // esp_now_send(nullptr, …) only delivers to already-registered unicast peers.
  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_err_t br =
      esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&beacon), sizeof(beacon));
  Logger::logln("MESH", String("Beacon broadcast ") + (br == ESP_OK ? "OK" : "FAIL"),
                LogLevel::LOG_DEBUG);
}

void Mesh::loadMeshKeyFromEEPROM() {
  // Attempt to load mesh key from EEPROM
  if (!EepromManager::getInstance().loadMeshKey(meshKey, MESH_KEY_SIZE)) {
    Logger::logln("MESH", "EEPROM read failed, using default mesh key", LogLevel::LOG_WARN);
  }

  // If in DEV_MODE always override with compile-time key
  if (lattice::config::DEV_MODE) {
    memcpy(meshKey, lattice::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    Logger::logln("MESH", "DEV_MODE: Overriding mesh key with compile-time default",
                  LogLevel::LOG_INFO);
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
    memcpy(meshKey, lattice::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    saveMeshKeyToEEPROM(meshKey); // Will be skipped automatically in dev mode
  }
}

void Mesh::saveMeshKeyToEEPROM(const uint8_t* key) {
  EepromManager::getInstance().saveMeshKey(key, MESH_KEY_SIZE);
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
    if (meshKey[i] != 0xFF)
      return true;
  return false;
}

void Mesh::broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memset(msg.target_mac_address, 0xFF, 6); // broadcast indicator — relayed by intermediate nodes
  broadcastToAllPeers(msg);
  if (deliverLocally && externalRecvCallback) {
    externalRecvCallback(msg);
  }
}

void Mesh::broadcastAdapterDataStatic(adapter_types type, const uint8_t* data) {
  if (instance)
    instance->broadcastAdapterData(type, data);
}

void Mesh::debugDumpRadio() {
  if (!EepromManager::getInstance().getDevMode())
    return;
  uint8_t ch;
  esp_wifi_get_channel(&ch, nullptr);
  String out = String("DBG Channel=") + String(ch) + " Key=";
  for (int i = 0; i < MESH_KEY_SIZE; ++i) {
    if (meshKey[i] < 0x10)
      out += "0";
    out += String(meshKey[i], HEX) + " ";
  }
  Logger::logln("MESH", out, LogLevel::LOG_INFO);
}

void Mesh::checkMasterTimeout() {
  if (isMaster)
    return;
  if (currentMaster.distance == 0xFF)
    return; // No master known yet
  if (millis() - lastMasterBeaconReceivedMs > STALE_MASTER_THRESHOLD_MS) {
    Logger::logln("MESH", "Master beacon timeout — clearing route, treating as offline",
                  LogLevel::LOG_WARN);
    memset(currentMaster.mac, 0, 6);
    currentMaster.distance = 0xFF;
    memset(currentMaster.nextHop, 0, 6);
    memset(lastSeenMasterMac, 0, 6);
    lastMasterBeaconReceivedMs = 0;
  }
}

// ---------- Tiger Style helper implementations ----------

void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: ignore echoes of our own beacon relayed back by neighbours (relays are
  // broadcast, so the originating master hears them too). Without this the master
  // would TOFU-learn itself as knownMasterMac and record a bogus route to itself.
  if (memcmp(msg.origin_mac_address, deviceMacAddress, 6) == 0)
    return;

  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    Logger::logln("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // --- TOFU master MAC enforcement ---
  bool fromPrimary =
      enrollment.hasMasterMac && memcmp(msg.origin_mac_address, enrollment.knownMasterMac, 6) == 0;
  bool fromSecondary = _dualMasterMode && enrollment.hasMasterMacSecondary &&
                       memcmp(msg.origin_mac_address, enrollment.knownMasterMacSecondary, 6) == 0;

  if (!enrollment.hasMasterMac) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    memcpy(enrollment.knownMasterMac, msg.origin_mac_address, 6);
    enrollment.hasMasterMac = true;
    EepromManager::getInstance().saveKnownMasterMac(enrollment.knownMasterMac);
    Logger::logln("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  } else if (!fromPrimary && !fromSecondary) {
    // Beacon from unrecognised MAC
    if (_dualMasterMode && !enrollment.hasMasterMacSecondary) {
      // Second master TOFU — learn and save as secondary
      memcpy(enrollment.knownMasterMacSecondary, msg.origin_mac_address, 6);
      enrollment.hasMasterMacSecondary = true;
      EepromManager::getInstance().saveKnownMasterMacSecondary(enrollment.knownMasterMacSecondary);
      Logger::logln("MESH", "Secondary master MAC learned (TOFU)", LogLevel::LOG_INFO);
      // fall through to process this beacon as valid
    } else if (millis() - lastMasterBeaconReceivedMs < STALE_MASTER_THRESHOLD_MS) {
      // Known master(s) still fresh — reject unknown MAC
      Logger::logln("MESH", "Beacon from unexpected MAC rejected (master still alive)",
                    LogLevel::LOG_WARN);
      return;
    } else {
      // All known masters stale — accept as new primary (hotswap)
      Logger::logln("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
      memcpy(enrollment.knownMasterMac, msg.origin_mac_address, 6);
      EepromManager::getInstance().saveKnownMasterMac(enrollment.knownMasterMac);
    }
  }

  if (lattice::utils::MacAddress(lastSeenMasterMac) !=
          lattice::utils::MacAddress(msg.origin_mac_address) &&
      lastSeenMasterMac[0] != 0) {
    if (_dualMasterMode) {
      Logger::logln("MESH", "Two masters active (dual master mode)", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
      lattice::err::fail(
          lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 7,
          "Multiple master nodes detected! Network split or misconfiguration likely.");
    }
  }
  memcpy(lastSeenMasterMac, msg.origin_mac_address, 6);
  lastMasterBeaconReceivedMs = millis();

  uint8_t newDistance = msg.hop_count + 1;
  if (currentMaster.distance == 0xFF ||
      lattice::utils::MacAddress(currentMaster.mac) !=
          lattice::utils::MacAddress(msg.origin_mac_address) ||
      newDistance < currentMaster.distance) {
    memcpy(currentMaster.mac, msg.origin_mac_address, 6);
    currentMaster.distance = newDistance;
    memcpy(currentMaster.nextHop, msg.last_hop_mac_address, 6);
    Logger::logln("MESH", "Updated route to master. Distance: " + String(newDistance),
                  LogLevel::LOG_INFO);
  }

  // Multi-hop routing (spec §3): the node we heard this beacon THROUGH
  // (last_hop) is a forwarding candidate. msg.hop_count is last_hop's OWN
  // distance to the master (this receiving node's distance is one more, per
  // `newDistance` above — last_hop is one hop closer), so last_hop's distance
  // is msg.hop_count, not +1: a direct beacon straight from the master
  // (hop_count == 0, last_hop == master) must record the master itself as a
  // distance-0 neighbor. Learned here, not from enrollment — routing only.
  neighbors.observe(msg.last_hop_mac_address, msg.hop_count, millis());

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    bool isNewer =
        (msg.epoch_num > replay.lastRelayedEpoch) ||
        (msg.epoch_num == replay.lastRelayedEpoch && msg.seq_num > replay.lastRelayedSeqNum);
    if (!isNewer) {
      Logger::logln("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    replay.lastRelayedEpoch = msg.epoch_num;
    replay.lastRelayedSeqNum = msg.seq_num;

    // Defer relay with random jitter to stagger transmissions across all non-master
    // nodes and eliminate the collision burst that occurs when all nodes relay
    // within milliseconds of receiving the same beacon.
    // Jitter window: 10–73 ms (10 + esp_random() % RELAY_JITTER_MAX_MS)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % lattice::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hop_count = newDistance;
    memcpy(relayPendingMsg.last_hop_mac_address, deviceMacAddress, 6);
    relayPendingAt = millis() + 10 + jitterMs;
    relayPending = true;
  }
}

void Mesh::processAdapterData(const mesh_message& msg) {
  // OP_CONFIG_SET = 0xC1 (from lib/lattice-protocol/opcodes.h)
  static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  bool addressedToSelf = (memcmp(msg.target_mac_address, deviceMacAddress, 6) == 0);
  bool isBroadcastTarget = (memcmp(msg.target_mac_address, kBroadcastMac, 6) == 0);
  bool addressedToMaster =
      enrollment.hasMasterMac && (memcmp(msg.target_mac_address, currentMaster.mac, 6) == 0);

  if (!isMaster && !addressedToSelf && !isBroadcastTarget) {
    if (addressedToMaster) {
      // Uplink: relay toward master via routing table
      if (msg.hop_count >= lattice::config::MAX_HOPS)
        return;
      mesh_message relay = msg;
      relay.hop_count++;
      memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
      transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ADAPTER_DATA,
                   &relay);
      return;
    }
    // Downlink to another node: relay outward toward specific target
    relayDownlink(msg);
    return;
  }

  // Security gate: at the master, a sealed-type frame (ADAPTER_DATA/ROUTE_REPORT)
  // that is NOT addressed to self must never reach local delivery unopened. No
  // leaf ever originates a broadcast-target (FF:FF:FF:FF:FF:FF) sealed uplink —
  // only the master's own downlink broadcast (broadcastAdapterData, which delivers
  // locally directly and never re-enters this function) and beacons use FF:FF. So
  // a broadcast-target (or otherwise not-self-addressed) sealed frame arriving here
  // over the air at the master is either a stale self-echo or a forgery — drop it
  // rather than deliver it to externalRecvCallback without E2E authentication.
  if (isMaster && !addressedToSelf && isSealedType(msg.message_type)) {
    Logger::logln("MESH",
                  "Master: sealed-type frame not addressed to self rejected (unauthenticated)",
                  LogLevel::LOG_WARN);
    return;
  }

  // Local delivery
  // E2E open (spec §2): master unseals self-targeted uplink before local delivery.
  mesh_message opened = msg;
  bool needsOpen = isMaster && addressedToSelf && isSealedType(msg.message_type);
  if (needsOpen) {
    const uint8_t *kUp, *kDown;
    if (!peerE2EKeys(msg.origin_mac_address, &kUp, &kDown) ||
        !lattice::mesh::crypto::openPayload(kUp, opened)) {
      Logger::logln("MESH", "E2E open failed — frame dropped", LogLevel::LOG_WARN);
      return;
    }
  }

  bool isConfigOpcode =
      (opened.data_type == adapter_types::SERIAL_ADAPTER && opened.data[0] == OP_CONFIG_SET);
  // TODO(dual-master): also allow secondary master MAC for CONFIG_SET
  if (isConfigOpcode && enrollment.hasMasterMac &&
      memcmp(opened.origin_mac_address, enrollment.knownMasterMac, 6) != 0) {
    Logger::logln("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
    return;
  }
  // Note: the "master received ADAPTER_DATA not addressed to self" case is now
  // handled (and rejected) by the security gate above — ADAPTER_DATA is always a
  // sealed type, so isMaster && !addressedToSelf never reaches this point.
  if (externalRecvCallback)
    externalRecvCallback(opened);

  // Broadcast: also relay so multi-hop nodes receive it (Task 3 test covers this)
  if (isBroadcastTarget && !isMaster) {
    relayDownlink(msg);
  }
}

void Mesh::relayDownlink(const mesh_message& msg) {
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  for (size_t i = 0; i < peers.peerCount; ++i) {
    if (memcmp(peers.peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue;
    sendMessage(peers.peerMacs[i].mac, relay);
  }
}

void Mesh::relayEnrollmentUplink(const mesh_message& msg) {
  // Never relay our own outbound request echoed back to us over the air.
  if (memcmp(msg.origin_mac_address, deviceMacAddress, 6) == 0)
    return;
  // Bound relay depth (mirrors the ADAPTER_DATA uplink guard).
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  // Can only relay toward the master if we actually have a route to it.
  if (!findNextHopToMaster())
    return;
  // Relay one hop toward the master, exactly like the ADAPTER_DATA uplink path:
  // bump hop_count, stamp ourselves as last hop, and route via findNextHopToMaster
  // (transmitCore does NOT rewrite target for non-ADAPTER_DATA types, so the
  // request's broadcast target is preserved for the master to process).
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ENROLLMENT,
               &relay);
}

void Mesh::processJoinAck(const mesh_message& msg) {
  // Relay outward if not addressed to us (multi-hop enrollment, Task 9b Bug #5
  // downlink counterpart). The target node is still mid-enrollment and is NOT
  // yet a registered unicast peer of ours, so — exactly as the master does when
  // it first emits the ACK (see enrollPeer: broadcast via the FF:FF peer) — we
  // RE-BROADCAST rather than unicast to known peers via relayDownlink(). Loop
  // safety: never re-broadcast a JOIN_ACK we originated (only masters originate
  // them, so this stops the master looping on its own echo), and bound depth by
  // MAX_HOPS as a backstop for cyclic topologies.
  if (memcmp(msg.target_mac_address, deviceMacAddress, 6) != 0) {
    if (memcmp(msg.origin_mac_address, deviceMacAddress, 6) == 0)
      return;
    if (msg.hop_count >= lattice::config::MAX_HOPS)
      return;
    mesh_message relay = msg;
    relay.hop_count++;
    memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
    static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&relay), sizeof(relay));
    return;
  }
  // Masters issue JOIN_ACKs; they never enroll via one. Without this guard a
  // forged ACK addressed to the master (fingerprint is observable over the
  // air) could TOFU-poison it and register attacker key material.
  if (isMaster) {
    Logger::logln("MESH", "JOIN_ACK addressed to master — ignoring", LogLevel::LOG_WARN);
    return;
  }
  enrollment.processJoinAck(msg, deviceMacAddress,
                            [this](const uint8_t* mac, const uint8_t* pubKey32) {
                              return registerPeerWithKey(mac, pubKey32, /*allowRekey=*/false);
                            });
}

void Mesh::addPeer(const uint8_t* mac) {
  size_t before = peers.peerCount;
  peers.addAndPersist(mac);
  if (peers.peerCount > before) {
    lattice::mesh::crypto::registerPeerWithEspNow(peers.peerMacs[peers.peerCount - 1].mac);
  }
}

bool Mesh::registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey) {
  PeerInfo* p = peers.find(mac);
  if (p) {
    if (!allowRekey) {
      // Established (non-zero) key material must never be replaced from this
      // path — an over-the-air JOIN_ACK with a spoofed trusted origin would
      // otherwise re-key the link to attacker-chosen material. An all-zero
      // stored key is a pre-enrollment placeholder (e.g. DEFAULT_PEERS), not
      // an established key, so upgrading it is allowed.
      bool keyEstablished = false;
      for (int i = 0; i < 32; ++i) {
        if (p->publicKey[i] != 0) {
          keyEstablished = true;
          break;
        }
      }
      if (keyEstablished) {
        Logger::logln("MESH", "Peer already registered — keeping established key",
                      LogLevel::LOG_DEBUG);
        p->lastSeenMillis = millis();
        return true; // already routable; nothing to change
      }
    }
    // Update existing peer's public key
    memcpy(p->publicKey, publicKey32, 32);
    p->lastSeenMillis = millis();
  } else {
    if (peers.peerCount >= MAX_PEERS) {
      Logger::logln("MESH", "Peer list full, cannot enroll", LogLevel::LOG_WARN);
      return false;
    }
    PeerInfo newPeer;
    memcpy(newPeer.mac, mac, 6);
    memcpy(newPeer.publicKey, publicKey32, 32);
    newPeer.lastSeenMillis = millis();
    peers.append(newPeer);
  }
  peers.saveToEEPROM();

  // Re-register with encryption now that we have the public key (mbedtls-heavy via Enrollment)
  enrollment.enrollPeer(mac, publicKey32, nullptr, _dualMasterMode);
  return true;
}

void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32) {
  if (!registerPeerWithKey(mac, publicKey32, /*allowRekey=*/true))
    return; // registry full — do not ACK an enrollment we could not record

  // Send JOIN_ACK unicast to new node
  mesh_message ack = {};
  // Stamp proto_version + (epoch, seq) so the existing ReplayCache dedups
  // re-broadcast copies of this ACK (Task 9c R2): each relay node re-broadcasts a
  // given JOIN_ACK at most once (the reflected copy is dropped by isReplay before
  // processJoinAck), preventing combinatorial broadcast amplification.
  ack.proto_version = PROTO_VERSION;
  // Draw seq via the guarded choke point FIRST — it may bump replay.bootEpoch
  // on wrap — then stamp epoch_num from the (possibly just-bumped) value so
  // the ACK's epoch always matches the epoch its seq_num was drawn under.
  ack.seq_num = nextSeqGuarded();
  ack.epoch_num = replay.bootEpoch;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  ack.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(ack.origin_mac_address, deviceMacAddress, 6);
  memcpy(ack.target_mac_address, mac, 6);
  memcpy(ack.last_hop_mac_address, deviceMacAddress, 6);
  ack.hop_count = 0;
  // Include first 4 bytes of approved node's pubkey as fingerprint
  memcpy(ack.data, publicKey32, 4);
  // Include OUR public key so the enrolling node can register this master as
  // an encrypted, routable peer in its own registry (see Enrollment::processJoinAck).
  memcpy(ack.enrollment_public_key, enrollment.getPublicKey(), 32);
  // Broadcast via the registered FF:FF:… peer so the new node receives the ACK
  // even before it is individually registered as a unicast peer.
  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
  Logger::logln("MESH", "JOIN_ACK sent to newly enrolled node", LogLevel::LOG_INFO);
}
// --------------------------------------------------------

bool Mesh::sendRouteReport() {
  if (isMaster)
    return false;
  if (!findNextHopToMaster())
    return false;
  uint8_t data[64] = {};
  data[0] = OP_ROUTE_REPORT;
  data[1] = 0; // path_len — reserved; relays no longer accumulate here (spec §4)
  transmitCore(adapter_types::UNKNOWN_ADAPTER, data, MESH_TYPE_ROUTE_REPORT);
  return true;
}

void Mesh::processRouteReport(const mesh_message& msg) {
  if (isMaster) {
    // E2E open (spec §2): master unseals self-targeted uplink before parsing
    // the opcode/path bytes — the payload is ciphertext until opened.
    mesh_message opened = msg;
    const uint8_t *kUp, *kDown;
    if (!peerE2EKeys(msg.origin_mac_address, &kUp, &kDown) ||
        !lattice::mesh::crypto::openPayload(kUp, opened)) {
      Logger::logln("MESH", "E2E open failed — route report dropped", LogLevel::LOG_WARN);
      return;
    }
    if (opened.data[0] != OP_ROUTE_REPORT) {
      Logger::logln("MESH", "processRouteReport: bad opcode, dropping", LogLevel::LOG_WARN);
      return;
    }
    // Terminal endpoint — deliver to server via external callback
    if (externalRecvCallback)
      externalRecvCallback(opened);
    return;
  }

  // Relay node (spec §4): the payload is E2E-sealed end-to-end (origin -> master)
  // so a relay cannot read or mutate msg.data — path accumulation via data[] is
  // removed; it moves to the header route_path field in a future phase. Forward
  // the frame unmodified except routing metadata (hop_count/last_hop).
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    Logger::logln("MESH", "processRouteReport: hop limit reached, dropping", LogLevel::LOG_WARN);
    return;
  }

  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);

  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ROUTE_REPORT,
               &relay);
}

void Mesh::loop() {
  drainRecvQueue();
  EepromManager::getInstance().flushIfDirty();

  // Drain enrollment relay queued from ESP-NOW receive callback (WiFi task context).
  // Serial.write() must not be called from that callback — safe to do here in loop().
  enrollment.drainPendingRelay();

  {
    uint32_t now = millis();
    if (!isMaster && now - lastRouteReportMs >= lattice::config::ROUTE_REPORT_INTERVAL_MS) {
      if (sendRouteReport())
        lastRouteReportMs = now;
    }
  }

  // Deferred beacon relay with jitter: dispatch once the per-node jitter window expires.
  // This spreads relay transmissions across all non-master nodes to avoid collision bursts.
  // Beacons propagate AWAY from the master for route discovery, so the relay must be a
  // broadcast: routing it through transmitCore()/findNextHopToMaster() sent it back toward
  // the master (backwards — nodes 2+ hops out never heard it) and raised a spurious
  // err::fail every beacon interval on any node without a route (e.g. pre-enrollment).
  if (relayPending && millis() >= relayPendingAt) {
    relayPending = false;
    static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t res = esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&relayPendingMsg),
                                 sizeof(relayPendingMsg));
    if (res != ESP_OK) {
      Logger::logln("MESH", String("Beacon relay broadcast failed: ") + esp_err_to_name(res),
                    LogLevel::LOG_WARN);
    }
  }

  // Master beacon — broadcastMasterBeacon() guards timing internally via lastBeaconMillis
  if (isMaster) {
    broadcastMasterBeacon();
  }
}

} // namespace mesh
} // namespace lattice
