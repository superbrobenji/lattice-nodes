#include "Mesh.h"
#include "src/network/MacAddress.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"  // unified error
#include "src/persistence/EEPROM_Manager.h"
#include "src/Adapter/Serial_Adapter/Serial_Adapter.h"  // for relayEnrollmentToServer
// Error.h already provides ERROR_CHECK macros
#include <esp_now.h>
#include <WiFi.h>
#include <cstring>
#include "../../project_config.h"
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

namespace planetopia {
namespace mesh {

using namespace planetopia::utils;

Mesh* Mesh::instance = nullptr;

// no longer need macEquals helper – use MacAddress equality directly

// Derive a 16-byte LMK for a peer using ECDH + SHA256.
// LMK = SHA256(ECDH_shared_secret || "planetopia-lmk")[0:16]
static void derivePeerLMK(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32, uint8_t* lmk16Out) {
  mbedtls_ecdh_context ecdh;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecdh_init(&ecdh);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  int ret = 0;

  const char* pers = "planetopia_ecdh";
  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               reinterpret_cast<const uint8_t*>(pers), strlen(pers));
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 10,
                           "MESH: derivePeerLMK — ctr_drbg_seed failed");
  }

  ret = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_CURVE25519);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 11,
                           "MESH: derivePeerLMK — ecdh_setup failed");
  }

  // Load own private key and peer public key (X coordinate only for Curve25519)
  ret = mbedtls_mpi_read_binary(&ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(d),
                                 ownPrivateKey32, 32);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 12,
                           "MESH: derivePeerLMK — mpi_read_binary (private key) failed");
  }

  ret = mbedtls_mpi_read_binary(&ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(X),
                                 peerPublicKey32, 32);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 13,
                           "MESH: derivePeerLMK — mpi_read_binary (peer public key) failed");
  }

  ret = mbedtls_mpi_lset(&ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(Z), 1);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 14,
                           "MESH: derivePeerLMK — mpi_lset (Qp.Z) failed");
  }

  uint8_t sharedSecret[32] = {};
  size_t outLen = 0;
  ret = mbedtls_ecdh_calc_secret(&ecdh, &outLen, sharedSecret, sizeof(sharedSecret),
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 15,
                           "MESH: derivePeerLMK — ecdh_calc_secret failed");
  }

  mbedtls_ecdh_free(&ecdh);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);

  // KDF: SHA256(sharedSecret || "planetopia-lmk"), take first 16 bytes
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);

  ret = mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256, not SHA-224
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 16,
                           "MESH: derivePeerLMK — sha256_starts failed");
  }

  ret = mbedtls_sha256_update(&sha, sharedSecret, 32);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 17,
                           "MESH: derivePeerLMK — sha256_update (secret) failed");
  }

  const uint8_t label[] = "planetopia-lmk";
  ret = mbedtls_sha256_update(&sha, label, sizeof(label) - 1);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 18,
                           "MESH: derivePeerLMK — sha256_update (label) failed");
  }

  uint8_t digest[32];
  ret = mbedtls_sha256_finish(&sha, digest);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 19,
                           "MESH: derivePeerLMK — sha256_finish failed");
  }

  mbedtls_sha256_free(&sha);

  memcpy(lmk16Out, digest, 16);

  // Zero sensitive buffers after use (Fix 2)
  memset(sharedSecret, 0, sizeof(sharedSecret));
  memset(digest, 0, sizeof(digest));
}

static void registerPeerWithEspNow(const uint8_t mac[6], const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32) {
  if (esp_now_is_peer_exist(mac)) return;
  uint8_t lmk[16];
  bool hasPublicKey = false;
  if (peerPublicKey32) {
    // Check that public key is not all-zero (unset)
    for (int i = 0; i < 32; ++i) {
      if (peerPublicKey32[i] != 0x00) { hasPublicKey = true; break; }
    }
  }
  if (hasPublicKey && ownPrivateKey32) {
    derivePeerLMK(ownPrivateKey32, peerPublicKey32, lmk);
  } else {
    // Peer public key not yet known (pre-enrollment) — no encryption
    memset(lmk, 0, 16);
  }
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = hasPublicKey;
  if (hasPublicKey) memcpy(info.lmk, lmk, 16);
  planetopia::err::checkEsp(esp_now_add_peer(&info), planetopia::utils::ErrorType::COMMUNICATION_FAIL, "registerPeerWithEspNow: add_peer failed");
}

Mesh::Mesh()
  : isMaster(false), lastBeaconMillis(0), lastMasterBeaconReceivedMs(0),
    bootEpoch(0), txSeqNum(0), replayCacheIdx(0),
    lastRelayedEpoch(0), lastRelayedSeqNum(0),
    hasMasterMac(false), peerCount(0),
    recvQueueHead(0), recvQueueTail(0), lastBeaconMs(0),
    relayPending(false), relayPendingAt(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(currentMaster.nextHop, 0, 6);
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  memset(devicePrivateKey, 0, 32);
  memset(devicePublicKey, 0, 32);
  memset(replayCache, 0, sizeof(replayCache));
  memset(knownMasterMac, 0xFF, 6);
  memset(peerMacs, 0, sizeof(peerMacs));
  memset(recvQueue, 0, sizeof(recvQueue));
  memset(&relayPendingMsg, 0, sizeof(relayPendingMsg));
}

bool Mesh::appendPeer(const PeerInfo& peer) {
  if (peerCount >= MAX_PEERS) return false;
  peerMacs[peerCount++] = peer;
  return true;
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
  peerCount = 0;

  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key
  uint8_t peerRecords[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_RECORD_SIZE];
  bool eepromOk = EEPROM_Manager::getInstance().loadPeerList(peerRecords, EEPROM_SIZES::MAX_PEERS);

  if (eepromOk) {
    for (int i = 0; i < EEPROM_SIZES::MAX_PEERS; ++i) {
      const uint8_t* record = peerRecords + (i * EEPROM_SIZES::PEER_RECORD_SIZE);
      // Treat all-0xFF MAC as empty slot
      bool valid = false;
      for (int j = 0; j < 6; ++j) {
        if (record[j] != 0xFF) { valid = true; break; }
      }
      if (valid) {
        PeerInfo peer;
        memcpy(peer.mac, record, 6);
        memcpy(peer.publicKey, record + 6, 32);
        peer.lastSeenMillis = 0;
        appendPeer(peer);
      }
    }
  }

  // Fallback in dev mode or when list is empty
  if (peerCount == 0) {
    Logger::logln("MESH", "Peer list empty; loading defaults from config", LogLevel::LOG_INFO);
    for (int i = 0; i < planetopia::config::NUM_DEFAULT_PEERS; ++i) {
      PeerInfo peer;
      memcpy(peer.mac, planetopia::config::DEFAULT_PEERS[i], 6);
      memset(peer.publicKey, 0, 32);  // Public key not known yet for config defaults
      peer.lastSeenMillis = 0;
      appendPeer(peer);
    }
  }
}

void Mesh::savePeersToEEPROM() {
  // Each record is PEER_RECORD_SIZE (38) bytes: 6 MAC + 32 public key
  uint8_t peerRecords[EEPROM_SIZES::MAX_PEERS * EEPROM_SIZES::PEER_RECORD_SIZE];
  memset(peerRecords, 0xFF, sizeof(peerRecords));

  for (size_t i = 0; i < peerCount && i < EEPROM_SIZES::MAX_PEERS; ++i) {
    uint8_t* record = peerRecords + (i * EEPROM_SIZES::PEER_RECORD_SIZE);
    memcpy(record, peerMacs[i].mac, 6);
    memcpy(record + 6, peerMacs[i].publicKey, 32);
  }

  EEPROM_Manager::getInstance().savePeerList(peerRecords, peerCount);
}

void Mesh::addPeerToEEPROM(const uint8_t mac[6]) {
  if (findPeer(mac) || planetopia::utils::MacAddress(mac) == planetopia::utils::MacAddress(deviceMacAddress)) return;

  if (peerCount >= MAX_PEERS) {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                         planetopia::core::ModuleDigit::MESH,
                         2,
                         "Peer list full! Cannot add new peer. MAX_PEERS reached.");
    return;
  }

  PeerInfo peer;
  memcpy(peer.mac, mac, 6);
  memset(peer.publicKey, 0, 32);  // Public key unknown until enrollment
  peer.lastSeenMillis = millis();
  appendPeer(peer);
  savePeersToEEPROM();
  registerPeerWithEspNow(peerMacs[peerCount - 1].mac, devicePrivateKey, peerMacs[peerCount - 1].publicKey);
  Logger::logln("MESH", "Peer added", LogLevel::LOG_DEBUG);
}

void Mesh::removePeerFromEEPROM(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (planetopia::utils::MacAddress(peerMacs[i].mac) == planetopia::utils::MacAddress(mac)) {
      peerMacs[i] = peerMacs[--peerCount];  // swap with last, shrink count
      break;
    }
  }
  savePeersToEEPROM();
  esp_err_t result = esp_now_del_peer(mac);
  planetopia::err::checkEsp(result, planetopia::utils::ErrorType::COMMUNICATION_FAIL, "removePeerFromEEPROM: del_peer failed");
  Logger::logln("MESH", "Removed ESP-NOW peer.", LogLevel::LOG_DEBUG);
}

PeerInfo* Mesh::findPeer(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (planetopia::utils::MacAddress(peerMacs[i].mac) == planetopia::utils::MacAddress(mac)) return &peerMacs[i];
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
  for (size_t i = 0; i < peerCount; ++i) {
    if (planetopia::utils::MacAddress(peerMacs[i].mac) == planetopia::utils::MacAddress(currentMaster.nextHop)
        && isPeerInRange(peerMacs[i].mac)
        && planetopia::utils::MacAddress(peerMacs[i].mac) != planetopia::utils::MacAddress(deviceMacAddress))
      return &peerMacs[i];
  }
  return nullptr;
}

mesh_message Mesh::buildMessage(adapter_types type, const uint8_t data[12], MeshMessageType msgType) {
  mesh_message msg = {};
  msg.protoVersion = PROTO_VERSION;
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
  msg.epochNum = bootEpoch;
  msg.seqNum = ++txSeqNum;
  return msg;
}

// ---------- Tiger Style init helpers ----------
bool Mesh::init() {
  // instance already set in constructor; no need to repeat
  // 1. Load persisted peers/keys
  loadPersistentState();

  // 2. Increment and save boot epoch (replay protection)
  bootEpoch = EEPROM_Manager::getInstance().loadBootEpoch() + 1;
  EEPROM_Manager::getInstance().saveBootEpoch(bootEpoch);
  txSeqNum = 0;
  memset(replayCache, 0, sizeof(replayCache));
  Logger::logln("MESH", "Boot epoch: " + String(bootEpoch), LogLevel::LOG_INFO);

  // 3. Configure Wi-Fi
  if (!setupWiFi()) return false;

  // 3a. Apply TX power preset from EEPROM (deployment-specific)
  {
    planetopia::config::TxPowerPreset preset = EEPROM_Manager::getInstance().loadTxPowerPreset();
    uint8_t txPowerVal = planetopia::config::TX_POWER_VALUES[static_cast<uint8_t>(preset)];
    esp_err_t txErr = esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerVal));
    if (txErr != ESP_OK) {
      Logger::logln("MESH", String("TX power set failed: ") + esp_err_to_name(txErr), LogLevel::LOG_WARN);
    } else {
      Logger::logln("MESH", "TX power preset applied", LogLevel::LOG_INFO);
    }
  }

  // 4. Init ESP-NOW
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
  loadOrGenerateKeypair();
  hasMasterMac = EEPROM_Manager::getInstance().loadKnownMasterMac(knownMasterMac);
  if (hasMasterMac) {
    Logger::logln("MESH", "Known master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
}

void Mesh::loadOrGenerateKeypair() {
  if (EEPROM_Manager::getInstance().loadKeypair(devicePrivateKey, devicePublicKey)) {
    Logger::logln("MESH", "Device keypair loaded from EEPROM", LogLevel::LOG_INFO);
    return;
  }

  Logger::logln("MESH", "Generating new Curve25519 keypair...", LogLevel::LOG_INFO);

  // Use the low-level ECP API directly to avoid the opaque ecdh context internals
  // that are private in the mbedTLS 3.x non-legacy context.
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point Q;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;

  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&Q);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  const char* pers = "planetopia_keygen";
  int ret;
  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               reinterpret_cast<const uint8_t*>(pers), strlen(pers));
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 1,
                           "MESH: keypair gen — entropy seed failed");
  }

  ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 2,
                           "MESH: keypair gen — ecp_group_load failed");
  }

  ret = mbedtls_ecdh_gen_public(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::MESH, 3,
                           "MESH: keypair gen — ecdh_gen_public failed");
  }

  // Export private scalar (d) — 32 bytes big-endian (NEVER printed to serial)
  mbedtls_mpi_write_binary(&d, devicePrivateKey, 32);
  // Export public key X coordinate — 32 bytes (Curve25519 public key is X only)
  mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), devicePublicKey, 32);

  mbedtls_ecp_group_free(&grp);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);

  EEPROM_Manager::getInstance().saveKeypair(devicePrivateKey, devicePublicKey);
  Logger::logln("MESH", "New keypair generated and saved", LogLevel::LOG_INFO);
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

  // Register the broadcast MAC so esp_now_send(broadcastMac, ...) reaches all
  // nodes — including unregistered ones. esp_now_send(nullptr, ...) only delivers
  // to already-registered peers; using the explicit FF:FF:… MAC is required for a
  // true 802.11 broadcast frame.
  static const uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  if (!esp_now_is_peer_exist(broadcastMac)) {
    esp_now_peer_info_t broadcast = {};
    memset(broadcast.peer_addr, 0xFF, 6);
    broadcast.channel = 0;
    broadcast.encrypt = false;
    esp_now_add_peer(&broadcast);
  }

  for (size_t i = 0; i < peerCount; ++i) {
    registerPeerWithEspNow(peerMacs[i].mac, devicePrivateKey, peerMacs[i].publicKey);
  }
  esp_now_register_send_cb(onDataSentCallback);
  esp_now_register_recv_cb(Mesh::dataRecvTrampoline);
  Logger::logln("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);
  return true;
}
// ------------------------------------------------


bool Mesh::isReplay(const mesh_message& msg) {
  for (size_t i = 0; i < REPLAY_CACHE_SIZE; ++i) {
    if (memcmp(replayCache[i].mac, msg.originMacAddress, 6) == 0 &&
        replayCache[i].epoch == msg.epochNum &&
        replayCache[i].seq == msg.seqNum) {
      return true;
    }
  }
  // Record this entry in the ring buffer
  memcpy(replayCache[replayCacheIdx].mac, msg.originMacAddress, 6);
  replayCache[replayCacheIdx].epoch = msg.epochNum;
  replayCache[replayCacheIdx].seq   = msg.seqNum;
  replayCacheIdx = (replayCacheIdx + 1) % REPLAY_CACHE_SIZE;
  return false;
}

void Mesh::onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status) {
  String statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail";
  Logger::logln("MESH", "Last Packet Send Status: " + statusStr, LogLevel::LOG_DEBUG);
}

void IRAM_ATTR Mesh::onDataRecvCallback(const esp_now_recv_info* info, const uint8_t* incomingData, int len) {
  if (!instance || !info || !incomingData) return;
  if (static_cast<size_t>(len) < sizeof(mesh_message)) return;

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

    // Proto version check
    if (msg.protoVersion != 0 && msg.protoVersion != PROTO_VERSION) {
      Logger::logln("MESH", "Unsupported proto version, dropping", LogLevel::LOG_WARN);
      continue;
    }

    // Replay check
    if (msg.protoVersion == PROTO_VERSION && msg.epochNum > 0) {
      if (isReplay(msg)) {
        Logger::logln("MESH", "Replayed message dropped", LogLevel::LOG_DEBUG);
        continue;
      }
    }

    // Update last-seen for known peers only (no EEPROM write — see Task 4)
    updatePeerLastSeen(entry.srcMac);

    switch (msg.messageType) {
    case MESH_TYPE_ENROLLMENT:    processEnrollmentRequest(msg); break;
    case MESH_TYPE_JOIN_ACK:      processJoinAck(msg);           break;
    case MESH_TYPE_MASTER_BEACON: processMasterBeacon(msg);      break;
    case MESH_TYPE_ADAPTER_DATA:  processAdapterData(msg);       break;
    default:
      Logger::logln("MESH", "Unknown message type, dropping", LogLevel::LOG_WARN);
    }
  }
}

void IRAM_ATTR Mesh::dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data, int len) {
  if (!instance) return;
  instance->onDataRecvCallback(mac_addr, data, len);
}

void Mesh::sendMessage(const uint8_t target[6], mesh_message msg) {
  if (planetopia::utils::MacAddress(target) == planetopia::utils::MacAddress(deviceMacAddress)) {
    Logger::logln("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  esp_err_t result = esp_now_send(target, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
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
  if (peerCount == 0) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0) continue;  // Skip self
    memcpy(msg.targetMacAddress, peerMacs[i].mac, 6);
    sendMessage(peerMacs[i].mac, msg);
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
  if (now - lastBeaconMillis < planetopia::config::MASTER_BEACON_INTERVAL_MS) return;
  lastBeaconMillis = now;

  mesh_message beacon = buildMessage(adapter_types::UNKNOWN_ADAPTER, nullptr, MESH_TYPE_MASTER_BEACON);
  beacon.data[0] = 1;  // protocolVersion
  beacon.hopCount = 0;

  // Broadcast-only: send to the registered FF:FF:… broadcast peer so the frame
  // reaches all nodes — including those not yet individually registered.
  // esp_now_send(nullptr, …) only delivers to already-registered unicast peers.
  static const uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_err_t br = esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&beacon), sizeof(beacon));
  Logger::logln("MESH", String("Beacon broadcast ") + (br == ESP_OK ? "OK" : "FAIL"), LogLevel::LOG_DEBUG);
}

// Optional peer management (can be used in your admin tools)
void Mesh::addPeer(const uint8_t mac[6]) {
  if (!findPeer(mac)) {
    if (peerCount >= MAX_PEERS) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                            planetopia::core::ModuleDigit::MESH,
                            6,
                            "Peer list full! Cannot add new peer. MAX_PEERS reached.");
      Logger::logln("MESH", "Peer list is full, skipping add", LogLevel::LOG_WARN);
      return;
    }
    PeerInfo p;
    memcpy(p.mac, mac, 6);
    memset(p.publicKey, 0, 32);  // Public key unknown until enrollment
    p.lastSeenMillis = 0;
    appendPeer(p);
    savePeersToEEPROM();
    registerPeerWithEspNow(peerMacs[peerCount - 1].mac, devicePrivateKey, peerMacs[peerCount - 1].publicKey);
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
  if (!EEPROM_Manager::getInstance().getDevMode()) return;
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
  if (millis() - lastMasterBeaconReceivedMs > STALE_MASTER_THRESHOLD_MS) {
    Logger::logln("MESH", "Master beacon timeout — clearing route, treating as offline", LogLevel::LOG_WARN);
    memset(currentMaster.mac, 0, 6);
    currentMaster.distance = 0xFF;
    memset(currentMaster.nextHop, 0, 6);
    memset(lastSeenMasterMac, 0, 6);
    lastMasterBeaconReceivedMs = 0;
  }
}

// ---------- Tiger Style helper implementations ----------
void Mesh::updatePeerLastSeen(const uint8_t mac[6]) {
  if (!mac) return;
  if (planetopia::utils::MacAddress(mac) == planetopia::utils::MacAddress(deviceMacAddress)) return;
  // Enrollment is the only path for new peers — do not auto-add unknown senders here.
  // Unknown senders are silently ignored for non-enrollment messages.
  PeerInfo* p = findPeer(mac);
  if (p) {
    p->lastSeenMillis = millis();
  }
}

void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hopCount >= planetopia::config::MAX_HOPS) {
    Logger::logln("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // TOFU master MAC enforcement
  if (hasMasterMac && memcmp(msg.originMacAddress, knownMasterMac, 6) != 0) {
    // Beacon from a different MAC than the known master
    if (millis() - lastMasterBeaconReceivedMs < STALE_MASTER_THRESHOLD_MS) {
      // Current master still fresh — reject the impostor
      Logger::logln("MESH", "Beacon from unexpected MAC rejected (master still alive)", LogLevel::LOG_WARN);
      return;
    }
    // Current master is stale — accept new master (master hotswap)
    Logger::logln("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
    memcpy(knownMasterMac, msg.originMacAddress, 6);
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
  } else if (!hasMasterMac) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    memcpy(knownMasterMac, msg.originMacAddress, 6);
    hasMasterMac = true;
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned from first beacon (TOFU fallback)", LogLevel::LOG_INFO);
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
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    bool isNewer = (msg.epochNum > lastRelayedEpoch) ||
                   (msg.epochNum == lastRelayedEpoch && msg.seqNum > lastRelayedSeqNum);
    if (!isNewer) {
      Logger::logln("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    lastRelayedEpoch  = msg.epochNum;
    lastRelayedSeqNum = msg.seqNum;

    // Defer relay with random jitter to stagger transmissions across all non-master
    // nodes and eliminate the collision burst that occurs when all nodes relay
    // within milliseconds of receiving the same beacon.
    // Jitter window: 10–73 ms (10 + esp_random() % RELAY_JITTER_MAX_MS)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % planetopia::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hopCount = newDistance;
    memcpy(relayPendingMsg.lastHopMacAddress, deviceMacAddress, 6);
    relayPendingAt = millis() + 10 + jitterMs;
    relayPending   = true;
  }
}

void Mesh::processAdapterData(const mesh_message& msg) {
  // Config-modifying opcodes are only accepted from the known master (S3 fix)
  static constexpr uint8_t OP_CONFIG_SET = 0xA0;
  bool isConfigOpcode = (msg.dataType == adapter_types::SERIAL_ADAPTER &&
                         msg.data[0] == OP_CONFIG_SET);
  if (isConfigOpcode && hasMasterMac &&
      memcmp(msg.originMacAddress, knownMasterMac, 6) != 0) {
    Logger::logln("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
    return;
  }
  if (externalRecvCallback) externalRecvCallback(msg);
}

bool Mesh::isEnrolled() const {
  return EEPROM_Manager::getInstance().loadEnrolledFlag();
}

void Mesh::sendEnrollmentRequest() {
  mesh_message msg = {};
  msg.messageType = MESH_TYPE_ENROLLMENT;
  msg.dataType = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.originMacAddress, deviceMacAddress, 6);
  memset(msg.targetMacAddress, 0xFF, 6);  // broadcast
  memcpy(msg.lastHopMacAddress, deviceMacAddress, 6);
  msg.hopCount = 0;

  static const uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  // Chunk 0: data[0]=0x00 (chunk index), data[1..11] = pubkey[0..10]
  msg.data[0] = 0x00;
  memcpy(&msg.data[1], devicePublicKey, 11);
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  delay(10);

  // Chunk 1: data[0]=0x01, data[1..11] = pubkey[11..21]
  msg.data[0] = 0x01;
  memcpy(&msg.data[1], devicePublicKey + 11, 11);
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  delay(10);

  // Chunk 2: data[0]=0x02, data[1..10] = pubkey[22..31], data[11]=0x00 (padding)
  msg.data[0] = 0x02;
  memcpy(&msg.data[1], devicePublicKey + 22, 10);
  msg.data[11] = 0x00;
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

  Logger::logln("MESH", "Enrollment request sent (3 chunks)", LogLevel::LOG_INFO);
}

void Mesh::processEnrollmentRequest(const mesh_message& msg) {
  if (!isMaster) {
    // Non-master nodes ignore enrollment requests (only master talks to server)
    return;
  }

  // Accumulate chunks from this MAC
  uint8_t chunkIdx = msg.data[0];
  const uint8_t* sender = msg.originMacAddress;

  // Static buffer: up to 4 concurrent enrolling nodes
  static struct { uint8_t mac[6]; uint8_t key[32]; uint8_t received; } buf[4] = {};
  int slot = -1;
  for (int i = 0; i < 4; ++i) {
    if (memcmp(buf[i].mac, sender, 6) == 0) { slot = i; break; }
  }
  if (slot < 0) {
    // Find an empty slot (received == 0 and mac is all-zero)
    for (int i = 0; i < 4; ++i) {
      bool empty = true;
      for (int j = 0; j < 6; ++j) { if (buf[i].mac[j] != 0) { empty = false; break; } }
      if (empty) { slot = i; break; }
    }
  }
  if (slot < 0) {
    Logger::logln("MESH", "Enrollment buffer full, dropping request", LogLevel::LOG_WARN);
    return;
  }
  memcpy(buf[slot].mac, sender, 6);

  if (chunkIdx == 0x00) {
    memcpy(buf[slot].key, &msg.data[1], 11);
    buf[slot].received |= 1;
  } else if (chunkIdx == 0x01) {
    memcpy(buf[slot].key + 11, &msg.data[1], 11);
    buf[slot].received |= 2;
  } else if (chunkIdx == 0x02) {
    memcpy(buf[slot].key + 22, &msg.data[1], 10);
    buf[slot].received |= 4;
  }

  if ((buf[slot].received & 0x07) == 0x07) {
    // All 3 chunks received — defer relay to loop() to avoid Serial.write() from callback context
    Logger::logln("MESH", "Enrollment request complete, deferring relay to loop()", LogLevel::LOG_INFO);
    memcpy(_pendingEnrollmentMac, buf[slot].mac, 6);
    memcpy(_pendingEnrollmentPubKey, buf[slot].key, 32);
    _pendingEnrollmentRelay = true;
    memset(&buf[slot], 0, sizeof(buf[slot]));  // Clear buffer slot
  }
}

void Mesh::processJoinAck(const mesh_message& msg) {
  // Verify the ack is addressed to us
  if (memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0) return;
  // Verify fingerprint matches our public key (first 4 bytes)
  if (memcmp(msg.data, devicePublicKey, 4) != 0) {
    Logger::logln("MESH", "JOIN_ACK fingerprint mismatch — ignoring", LogLevel::LOG_WARN);
    return;
  }
  Logger::logln("MESH", "Enrollment approved! Saving enrolled flag.", LogLevel::LOG_INFO);
  EEPROM_Manager::getInstance().saveEnrolledFlag(true);

  // The node sending JOIN_ACK is the master — record its MAC (TOFU)
  if (!hasMasterMac) {
    memcpy(knownMasterMac, msg.originMacAddress, 6);
    hasMasterMac = true;
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned and saved (TOFU)", LogLevel::LOG_INFO);
  }
}

void Mesh::enrollPeer(const uint8_t mac[6], const uint8_t publicKey32[32]) {
  PeerInfo* p = findPeer(mac);
  if (p) {
    // Update existing peer's public key
    memcpy(p->publicKey, publicKey32, 32);
  } else {
    if (peerCount >= MAX_PEERS) {
      Logger::logln("MESH", "Peer list full, cannot enroll", LogLevel::LOG_WARN);
      return;
    }
    PeerInfo newPeer;
    memcpy(newPeer.mac, mac, 6);
    memcpy(newPeer.publicKey, publicKey32, 32);
    newPeer.lastSeenMillis = 0;
    appendPeer(newPeer);
    p = &peerMacs[peerCount - 1];
  }
  savePeersToEEPROM();

  // Re-register with encryption now that we have the public key
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
  registerPeerWithEspNow(mac, devicePrivateKey, publicKey32);

  // Send JOIN_ACK unicast to new node
  mesh_message ack = {};
  ack.messageType = MESH_TYPE_JOIN_ACK;
  ack.dataType = adapter_types::UNKNOWN_ADAPTER;
  memcpy(ack.originMacAddress, deviceMacAddress, 6);
  memcpy(ack.targetMacAddress, mac, 6);
  memcpy(ack.lastHopMacAddress, deviceMacAddress, 6);
  ack.hopCount = 0;
  // Include first 4 bytes of approved node's pubkey as fingerprint
  memcpy(ack.data, publicKey32, 4);
  // Broadcast via the registered FF:FF:… peer so the new node receives the ACK
  // even before it is individually registered as a unicast peer.
  static const uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
  Logger::logln("MESH", "JOIN_ACK sent to newly enrolled node", LogLevel::LOG_INFO);
}
// --------------------------------------------------------

void Mesh::loop() {
  drainRecvQueue();
  EEPROM_Manager::getInstance().flushIfDirty();

  // Drain enrollment relay queued from ESP-NOW receive callback (WiFi task context).
  // Serial.write() must not be called from that callback — safe to do here in loop().
  if (_pendingEnrollmentRelay) {
    _pendingEnrollmentRelay = false;
    planetopia::adapter::Serial_Adapter::relayEnrollmentToServer(
        _pendingEnrollmentMac, _pendingEnrollmentPubKey);
  }

  // Deferred beacon relay with jitter: dispatch once the per-node jitter window expires.
  // This spreads relay transmissions across all non-master nodes to avoid collision bursts.
  if (relayPending && millis() >= relayPendingAt) {
    relayPending = false;
    transmitCore(relayPendingMsg.dataType, relayPendingMsg.data, MESH_TYPE_MASTER_BEACON, &relayPendingMsg);
  }

  // Master beacon — broadcastMasterBeacon() guards timing internally via lastBeaconMillis
  if (isMaster) {
    broadcastMasterBeacon();
  }
}

}
}
