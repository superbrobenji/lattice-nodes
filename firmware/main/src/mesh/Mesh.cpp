#include "Mesh.h"
#include "src/network/MacAddress.h"
#include "src/network/MacEq.h"
#include "src/network/hw_mac.h"
#include "src/network/mem.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h" // unified error
#include "src/persistence/EepromManager.h"
// Error.h already provides ERROR_CHECK macros
#include <esp_now.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include "../../project_config.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "MeshCrypto.h"
#include "E2ECrypto.h"
#include "RouteMac.h"
#include "broadcast_mac.h"
#include "config/master_pubkey_pin_wrapper.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

Mesh* Mesh::instance = nullptr;

// no longer need macEquals helper – use MacAddress equality directly

Mesh::Mesh()
    : isMaster(false), lastBeaconMillis(0), lastMasterBeaconReceivedMs(0), relayPendingAt(0),
      relayPending(false), _dualMasterMode(lattice::config::DUAL_MASTER_MODE), lastBeaconMs(0),
      lastRouteReportMs(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  memset(&relayPendingMsg, 0, sizeof(relayPendingMsg));
  // Phase I Task 8 (item OO): static ring buffer over _recvQueueStorage —
  // heap-free, matching the array it replaces. Storage is a Mesh member so
  // its lifetime matches the instance; safe to create unconditionally here
  // (doesn't depend on WiFi/ESP-NOW being up yet).
  recvQueue = xRingbufferCreateStatic(sizeof(_recvQueueStorage), RINGBUF_TYPE_NOSPLIT,
                                      _recvQueueStorage, &_recvQueueStruct);
}

// Seal-time AEAD nonce-reuse guard — see declaration comment in Mesh.h. Must
// be called immediately before every sealPayload() call-site with the
// (epoch, seq) about to be sealed; a rollback halts the node before the
// encryption call ever runs.
void Mesh::_checkEpochRollback(uint32_t epoch, uint16_t seq) {
  if (_lastSealedEpoch == UINT32_MAX) {
    _lastSealedEpoch = epoch;
    _lastSealedSeq = seq;
    return;
  }
  if (epoch > _lastSealedEpoch) {
    _lastSealedEpoch = epoch;
    _lastSealedSeq = seq;
    return;
  }
  if (epoch == _lastSealedEpoch && seq > _lastSealedSeq) {
    _lastSealedSeq = seq;
    return;
  }
  lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO, lattice::core::ModuleDigit::MESH, 1,
                     "AEAD epoch rollback — refusing seal");
}

void Mesh::readMacAddress() {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, deviceMacAddress);
  if (ret != ESP_OK) {
    // Phase I Task 7 (TT): String() temporary eliminated — stack buffer +
    // snprintf feeds err::fail's const char* directly.
    char errBuf[80];
    snprintf(errBuf, sizeof(errBuf), "MESH: Failed to read MAC address: %s", esp_err_to_name(ret));
    lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::MESH, 1,
                       errBuf);
  } else {
    // Prime the boot-time cache (item F) so every adapter's readOwnMac() call
    // is a memcpy instead of a repeat esp_wifi_get_mac() syscall.
    lattice::hw::cacheDeviceMac(deviceMacAddress);
    LATTICE_LOG("MESH", "Device MAC: ", LogLevel::LOG_DEBUG);
    LATTICE_LOGF("MESH", LogLevel::LOG_DEBUG, "%02X:%02X:%02X:%02X:%02X:%02X", deviceMacAddress[0],
                 deviceMacAddress[1], deviceMacAddress[2], deviceMacAddress[3], deviceMacAddress[4],
                 deviceMacAddress[5]);
  }
}

PeerInfo* Mesh::findNextHopToMaster() {
  if (currentMaster.distance == 0xFF)
    return nullptr;

  // Prefer an enrolled peer that is the direct master and in range (distance 1,
  // the common single-hop case) — keeps the existing behavior and E2E peering.
  PeerInfo* direct = peers.find(currentMaster.mac);
  if (direct && currentMaster.distance == 1 && peers.isPeerInRange(direct->mac) &&
      !lattice::mac::eq(direct->mac, deviceMacAddress))
    return direct;

  // Multi-hop (spec §3): pick the freshest neighbor strictly closer to the
  // master from the NeighborTable. The relay need not be an enrolled peer.
  uint8_t hopMac[6];
  if (!neighbors.selectNextHop(currentMaster.distance,
                               static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL, hopMac))
    return nullptr;
  if (lattice::mac::eq(hopMac, deviceMacAddress))
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
  bool isNewRelay = !lattice::mac::eq(forwardingPeer, hopMac);
  bool forwardingPeerSet = !lattice::mac::eq(forwardingPeer, kZeroMac);
  if (forwardingPeerSet && isNewRelay && !peers.find(forwardingPeer) &&
      !lattice::mac::eq(forwardingPeer, currentMaster.mac)) {
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

void Mesh::registerDownlinkPeer(const uint8_t* mac) {
  // Enrolled peers and the current master are managed exclusively by their
  // own paths (PeerRegistry / enrollment) — just register (idempotent) and
  // never track or evict them via this LRU.
  bool isCurrentMaster = currentMaster.distance != 0xFF && lattice::mac::eq(mac, currentMaster.mac);
  if (peers.find(mac) || isCurrentMaster) {
    // Defense-in-depth (issue #47 item 5): if this MAC was already parked in
    // the downlink forwarding-peer LRU from earlier churn (before it became
    // enrolled or the current master), evict it here — its peering is now
    // owned by PeerRegistry/enrollment, not this LRU. This branch is taken on
    // every call once a MAC is enrolled/master (it short-circuits ahead of
    // the LRU-touch loop below), so without this eviction a stale entry
    // would sit in downlinkPeerLru indefinitely instead of freeing its slot.
    for (size_t i = 0; i < downlinkPeerLruCount; ++i) {
      if (lattice::mac::eq(downlinkPeerLru[i], mac)) {
        for (size_t j = i; j + 1 < downlinkPeerLruCount; ++j)
          memcpy(downlinkPeerLru[j], downlinkPeerLru[j + 1], 6);
        downlinkPeerLruCount--;
        break;
      }
    }
    lattice::mesh::crypto::registerPeerWithEspNow(mac);
    return;
  }

  // Already tracked: touch (move to front) and ensure still registered.
  for (size_t i = 0; i < downlinkPeerLruCount; ++i) {
    if (lattice::mac::eq(downlinkPeerLru[i], mac)) {
      uint8_t touched[6];
      memcpy(touched, downlinkPeerLru[i], 6);
      for (size_t j = i; j > 0; --j)
        memcpy(downlinkPeerLru[j], downlinkPeerLru[j - 1], 6);
      memcpy(downlinkPeerLru[0], touched, 6);
      lattice::mesh::crypto::registerPeerWithEspNow(mac);
      return;
    }
  }

  // Not tracked. Evict the oldest (LRU) entry from ESP-NOW first if at
  // capacity (spec §2: "20-peer cap, LRU-evicted") — otherwise an RF attacker
  // crafting downlink frames with fresh distinct next-hop MACs on every frame
  // would grow this set unbounded and eventually exhaust the ESP-NOW peer
  // table (no self-heal, no reboot).
  if (downlinkPeerLruCount >= lattice::config::LATTICE_DOWNLINK_PEER_MAX) {
    uint8_t* oldest = downlinkPeerLru[downlinkPeerLruCount - 1];
    if (esp_now_is_peer_exist(oldest))
      esp_now_del_peer(oldest);
  } else {
    ++downlinkPeerLruCount;
  }
  for (size_t j = downlinkPeerLruCount - 1; j > 0; --j)
    memcpy(downlinkPeerLru[j], downlinkPeerLru[j - 1], 6);
  memcpy(downlinkPeerLru[0], mac, 6);
  lattice::mesh::crypto::registerPeerWithEspNow(mac);
}

uint16_t Mesh::nextSeqGuarded() {
  uint16_t seq = txState.nextSeq();
  if (seq == 0) {
    // seq wrapped (spec §2): a reused (epoch, seq) pair would reuse an AEAD nonce.
    // Advance the persisted epoch and restart the sequence. txState.bootEpoch is
    // already the currently active epoch (kept in sync with EEPROM by init()
    // and by this method), so bump from it directly rather than re-reading
    // EEPROM.
    uint32_t epoch = txState.bootEpoch + 1;
    lattice::eeprom::saveBootEpoch(epoch);
    txState.bumpEpoch(epoch);
    seq = txState.nextSeq();
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
  msg.epoch_num = txState.bootEpoch;
  return msg;
}

// ---------- Tiger Style init helpers ----------
bool Mesh::init() {
  // instance already set in constructor; no need to repeat
  // 1. Load persisted peers/keys
  loadPersistentState();

  // Allocate (or free) the RouteTable per the current role (issue #51) —
  // masters need it for downlink source routing, leaves never do.
  reevaluateRouteTable();

  // 2. Increment and save boot epoch (replay protection)
  uint32_t epoch = lattice::eeprom::loadBootEpoch() + 1;
  lattice::eeprom::saveBootEpoch(epoch);
  replay.init();
  txState.init(epoch);
  LATTICE_LOGF("MESH", LogLevel::LOG_INFO, "Boot epoch: %lu", (unsigned long)txState.bootEpoch);

  // Phase D (#42): DEV_MODE bypasses the beacon origin-MAC pin (dev firmware
  // regenerates a fresh keypair/MAC each boot, so pinning would reject the
  // dev master). Make that state visible in operator logs.
  if (lattice::config::DEV_MODE) {
    LATTICE_LOGLN("MESH", "DEV_MODE: master pubkey pin disabled — do not ship this build",
                  LogLevel::LOG_WARN);
  }

  // 3. Configure Wi-Fi
  if (!setupWiFi())
    return false;

  // 3a. Apply TX power preset from EEPROM (deployment-specific)
  {
    lattice::config::TxPowerPreset preset = lattice::eeprom::loadTxPowerPreset();
    uint8_t txPowerVal = lattice::config::TX_POWER_VALUES[static_cast<uint8_t>(preset)];
    esp_err_t txErr = esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerVal));
    if (txErr != ESP_OK) {
      LATTICE_LOGF("MESH", LogLevel::LOG_WARN, "TX power set failed: %s", esp_err_to_name(txErr));
    } else {
      LATTICE_LOGLN("MESH", "TX power preset applied", LogLevel::LOG_INFO);
    }
  }

  // 4. Init ESP-NOW
  if (!setupEspNow())
    return false;

  return true;
}

bool Mesh::setupWiFi() {
  // Phase I Task 3 (BB + ZZ): raw ESP-IDF WiFi bring-up, replacing the
  // arduino-esp32 WiFi.mode(WIFI_STA) wrapper. ESP-NOW is our only WiFi use
  // (no AP association, no persisted creds) so STA mode + WIFI_STORAGE_RAM
  // (making CONFIG_ESP_WIFI_NVS_ENABLED=n fully effective) is sufficient.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

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
    LATTICE_LOGLN("MESH", "Known master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
  if (_dualMasterMode && enrollment.hasMasterMacSecondary) {
    LATTICE_LOGLN("MESH", "Known secondary master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
}

bool Mesh::setupEspNow() {
  esp_err_t res = esp_now_init();
  if (res != ESP_OK) {
    // Phase I Task 7 (TT): String() temporary eliminated.
    char errBuf[80];
    snprintf(errBuf, sizeof(errBuf), "MESH: esp_now_init failed: %s", esp_err_to_name(res));
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 3,
                       errBuf);
    return false;
  }
  lattice::err::checkEsp(esp_now_set_pmk(meshKey), lattice::utils::ErrorType::HARDWARE_FAILURE,
                         "Failed to set ESP-NOW PMK");

  // Register the broadcast MAC so esp_now_send(BROADCAST_MAC, ...) reaches all
  // nodes — including unregistered ones. esp_now_send(nullptr, ...) only delivers
  // to already-registered peers; using the explicit FF:FF:… MAC is required for a
  // true 802.11 broadcast frame.
  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
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
  LATTICE_LOGLN("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);
  return true;
}
// ------------------------------------------------

void Mesh::onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status) {
  // Inlined into the LATTICE_LOGF call (rather than a local `statusStr`) so that
  // under LOG_NONE, where the whole call folds to ((void)0), there's no
  // now-unused local left behind to warn about.
  LATTICE_LOGF("MESH", LogLevel::LOG_DEBUG, "Last Packet Send Status: %s",
               (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail");
}

void IRAM_ATTR Mesh::onDataRecvCallback(const esp_now_recv_info* info, const uint8_t* incomingData,
                                        int len) {
  if (!instance || !info || !incomingData)
    return;
  if (static_cast<size_t>(len) < sizeof(mesh_message))
    return;

  RecvQueueEntry entry;
  memcpy(entry.srcMac, info->src_addr, 6);
  memcpy(&entry.msg, incomingData, sizeof(mesh_message));

  BaseType_t woken = pdFALSE;
  // Queue full — xRingbufferSendFromISR returns pdFALSE and the packet is
  // silently dropped, matching the old array's "Queue full — drop" behavior
  // (no logging here: this runs in WiFi task/ISR context, and Serial writes
  // are not safe from that context — see loop()'s comment on
  // drainPendingRelay).
  xRingbufferSendFromISR(instance->recvQueue, &entry, sizeof(entry), &woken);

  // Phase I Task 9 (item EE): wake the dedicated mesh-drain task instead of
  // leaving drain() to be discovered by a polling loop() iteration — this is
  // what lets loop() (and therefore the FreeRTOS idle task) go idle between
  // real work instead of busy-checking recvQueue every tick, which is a
  // prerequisite for tickless idle / light sleep to pay off. Null handle
  // (host/SimNode builds, or a real boot before setDrainNotifyHandle() has
  // run yet) is a no-op here — the item just waits in recvQueue for the next
  // explicit drain() call.
  BaseType_t woken2 = pdFALSE;
  if (instance->drainNotifyHandle_ != nullptr) {
    vTaskNotifyGiveFromISR(instance->drainNotifyHandle_, &woken2);
  }
  if (woken || woken2)
    portYIELD_FROM_ISR();
}

void Mesh::drainRecvQueue() {
  size_t itemSize = 0;
  RecvQueueEntry* entryPtr;
  while ((entryPtr = static_cast<RecvQueueEntry*>(xRingbufferReceive(recvQueue, &itemSize, 0))) !=
         nullptr) {
    if (itemSize != sizeof(RecvQueueEntry)) {
      // Should never happen (NOSPLIT items are always sent whole) — guard
      // against a corrupt/short item rather than reading past it.
      vRingbufferReturnItem(recvQueue, entryPtr);
      continue;
    }
    RecvQueueEntry entry = *entryPtr;
    vRingbufferReturnItem(recvQueue, entryPtr);

    const mesh_message& msg = entry.msg;

    // Proto version check: drop anything that isn't exactly the current wire
    // version. There is no legitimate proto_version==0 case — buildMessage(),
    // Enrollment::sendRequest(), and the JOIN_ACK path all stamp PROTO_VERSION
    // unconditionally — so a zero value only ever means a forged/malformed
    // frame that would otherwise bypass both this flag-day drop and the replay
    // gate below (which is itself keyed on proto_version == PROTO_VERSION).
    if (msg.proto_version != PROTO_VERSION) {
      LATTICE_LOGLN("MESH", "Unsupported proto version, dropping", LogLevel::LOG_WARN);
      continue;
    }

    // Replay check
    if (msg.proto_version == PROTO_VERSION && msg.epoch_num > 0) {
      if (replay.isReplay(msg, static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL)) {
        LATTICE_LOGLN("MESH", "Replayed message dropped", LogLevel::LOG_DEBUG);
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
      LATTICE_LOGLN("MESH", "Unknown message type, dropping", LogLevel::LOG_WARN);
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
  if (lattice::mac::eq(target, deviceMacAddress)) {
    LATTICE_LOGLN("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  esp_err_t result = esp_now_send(target, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (result == ESP_OK) {
    LATTICE_LOGLN("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    // Phase I Task 7 (TT): String() temporary eliminated.
    char errBuf[80];
    snprintf(errBuf, sizeof(errBuf), "MESH: Error sending message: %s", esp_err_to_name(result));
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 5,
                       errBuf);
  }
}

void Mesh::broadcastToAllPeers(const mesh_message& msg) {
  if (peers.peerCount == 0) {
    LATTICE_LOGLN("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peers.peerCount; ++i) {
    if (lattice::mac::eq(peers.peerMacs[i].mac, deviceMacAddress))
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

  bool selfOriginated = (lattice::mac::eq(msg.origin_mac_address, deviceMacAddress));

  // Only a self-originated uplink sets its own target to the master. A relayed
  // frame (msgOverride, foreign origin) is already sealed against the origin's
  // target — rewriting it would corrupt the AEAD AAD the destination master
  // verifies. Leave relayed frames' target untouched.
  if (msgType == MESH_TYPE_ADAPTER_DATA && selfOriginated) {
    memcpy(msg.target_mac_address, currentMaster.mac, 6);
  }

  // E2E seal (spec §1/§2): self-originated uplink payloads only. Relayed frames
  // (msgOverride with foreign origin) are already sealed — forward untouched.
  if (!isMaster && selfOriginated && isSealedType(msg.message_type)) {
    const uint8_t *kUp, *kDown;
    _checkEpochRollback(msg.epoch_num, msg.seq_num);
    if (!masterE2EKeys(&kUp, &kDown) || !lattice::mesh::crypto::sealPayload(kUp, msg)) {
      LATTICE_LOGLN("MESH", "E2E seal unavailable — uplink dropped", LogLevel::LOG_WARN);
      return;
    }

    // Chain-MAC seed (Phase C, spec §4 / issue #44): a self-originated route
    // report seeds msg.auth_path with this node's own hop in the chain (hop
    // 0 — prev_hop zeroed, no hop precedes the origin). Relays extend the
    // chain as they append to route_path (processRouteReport's relay
    // branch); the master reconstructs and verifies before recording the
    // route (processRouteReport's master branch). Reuses kUp already
    // derived above for the seal — same pairwise k_up with the master, no
    // new key material. Scoped to ROUTE_REPORT only: ADAPTER_DATA frames
    // don't carry a route_path to authenticate (see design doc non-goals —
    // no downlink-frame MAC).
    if (msg.message_type == MESH_TYPE_ROUTE_REPORT) {
      uint8_t prev_hop[6] = {0};
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, deviceMacAddress, ctx);
      uint8_t zero_prev_mac[routemac::AUTH_PATH_LEN] = {0};
      routemac::chainStep(kUp, ctx, zero_prev_mac, msg.auth_path);
    }
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop = findNextHopToMaster();
  if (nextHop && !lattice::mac::eq(nextHop->mac, deviceMacAddress)) {
    sendMessage(nextHop->mac, msg);
  } else {
    // No route to master is a routine, self-healing transient: a node that has
    // just booted (or whose master went stale) legitimately has no next hop
    // until it hears the next beacon. Drop the frame quietly rather than
    // escalating to err::fail — escalation here drives the error LED and
    // reboot-reason tracking, and turns every such gap (see
    // docs/design-gaps/multihop-data-uplink.md) into an error loop instead of a
    // silent drop. The upstream sender retries on its own timer.
    LATTICE_LOGLN("MESH", "No next hop to master — message dropped. Master timeout or unreachable.",
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
    LATTICE_LOGLN("MESH", "transmit() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->transmitDispatch(type, data, false);
}

void Mesh::transmitSelfOriginated(const adapter_types type, const uint8_t* data) {
  if (!instance) {
    LATTICE_LOGLN("MESH", "transmitSelfOriginated() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->transmitDispatch(type, data, true);
}

void Mesh::linkDataRecvCallback(std::function<void(const mesh_message&)> recvCallback) {
  externalRecvCallback = recvCallback;
}

// --- Periodically called in main loop if this node is master ---
void Mesh::broadcastMasterBeacon() {
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
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
  (void)sendBroadcast(beacon); // sendBroadcast already logs on failure
}

void Mesh::loadMeshKeyFromEEPROM() {
  // Attempt to load mesh key from EEPROM
  if (!lattice::eeprom::loadMeshKey(meshKey, MESH_KEY_SIZE)) {
    LATTICE_LOGLN("MESH", "EEPROM read failed, using default mesh key", LogLevel::LOG_WARN);
  }

  // If in DEV_MODE always override with compile-time key
  if (lattice::config::DEV_MODE) {
    memcpy(meshKey, lattice::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    LATTICE_LOGLN("MESH", "DEV_MODE: Overriding mesh key with compile-time default",
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
    LATTICE_LOGLN("MESH", "Mesh key unset, loading default from config", LogLevel::LOG_INFO);
    memcpy(meshKey, lattice::config::DEFAULT_MESH_KEY, MESH_KEY_SIZE);
    saveMeshKeyToEEPROM(meshKey); // Will be skipped automatically in dev mode
  }
}

void Mesh::saveMeshKeyToEEPROM(const uint8_t* key) {
  lattice::eeprom::saveMeshKey(key, MESH_KEY_SIZE);
}

void Mesh::broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memset(msg.target_mac_address, 0xFF, 6); // broadcast indicator — relayed by intermediate nodes
  broadcastToAllPeers(msg);
  if (deliverLocally && externalRecvCallback) {
    externalRecvCallback(msg);
  }
}

// Defense-in-depth (issue #47 item 4): true when a route path length would
// overflow route_path[]/MAX_HOPS bounds. RouteTable::record() already clamps
// pathLen at write time (parse-safety: `if (pathLen > config::MAX_HOPS) return;`
// in RouteTable.h), so this branch is not reachable via any current
// legitimate call path into sendDownlinkToNode() — routes->lookup() can only
// ever hand back a pathLen that record() previously accepted. The check below
// stays local to sendDownlinkToNode rather than relying solely on
// RouteTable's own guard, so the bound survives a future refactor of either
// side. Pure/stack-only (no allocation) — a free function (external linkage,
// not a Mesh member) so it stays directly unit-testable without needing to
// drive an integration path around RouteTable's guard, which is otherwise
// unreachable from outside RouteTable.h.
bool downlinkRouteLenExceedsMaxHops(uint8_t pathLen) {
  return pathLen > lattice::config::MAX_HOPS;
}

void Mesh::sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data) {
  if (!isMaster)
    return;
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memcpy(msg.target_mac_address, destMac, 6); // AAD-bound destination — set before sealing

  const uint8_t *kUp, *kDown;
  _checkEpochRollback(msg.epoch_num, msg.seq_num);
  if (!peerE2EKeys(destMac, &kUp, &kDown) || !lattice::mesh::crypto::sealPayload(kDown, msg)) {
    LATTICE_LOGLN("MESH", "downlink seal unavailable — dropped", LogLevel::LOG_WARN);
    return;
  }

  uint8_t path[lattice::config::MAX_HOPS * 6];
  uint8_t pathLen = 0;
  if (routes && routes->lookup(destMac, path, &pathLen) && pathLen > 0) {
    // Defensive clamp (issue #47 item 4) before indexing path[]/msg.route_path
    // with pathLen below — see downlinkRouteLenExceedsMaxHops() above.
    if (downlinkRouteLenExceedsMaxHops(pathLen)) {
      LATTICE_LOGLN("MESH", "downlink route_len exceeds MAX_HOPS — dropping", LogLevel::LOG_ERROR);
      return;
    }
    // RouteTable stores the path in origin->master order (as accumulated by
    // relays on the uplink route report); reverse it into master->origin order
    // for the downlink source route.
    msg.route_len = pathLen;
    for (uint8_t i = 0; i < pathLen; ++i)
      memcpy(&msg.route_path[static_cast<size_t>(i) * 6],
             &path[static_cast<size_t>(pathLen - 1 - i) * 6], 6);
    // First hop = route_path[0]; auto-register it as an unencrypted ESP-NOW peer
    // so esp_now_send can unicast to it — real ESP-NOW requires the peer to be
    // registered first (VirtualBus doesn't enforce this, but the Phase-2 lesson
    // was that skipping it here is a real-hardware bug). Bounded via the
    // downlink forwarding-peer LRU (spec §2) — see registerDownlinkPeer().
    registerDownlinkPeer(msg.route_path);
    sendMessage(msg.route_path, msg);
    return;
  }
  // No known multi-hop route: fall back to broadcast flood (still sealed).
  // Direct/adjacent nodes and unknown-route nodes are reached this way.
  msg.route_len = 0;
  broadcastToAllPeers(msg);
}

void Mesh::broadcastAdapterDataStatic(adapter_types type, const uint8_t* data) {
  if (instance)
    instance->broadcastAdapterData(type, data);
}

void Mesh::sendDownlinkToNodeStatic(const uint8_t* destMac, adapter_types type,
                                    const uint8_t* data) {
  if (instance)
    instance->sendDownlinkToNode(destMac, type, data);
}

bool Mesh::sendBroadcast(const mesh_message& msg) {
  esp_err_t err = esp_now_send(BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (err != ESP_OK) {
    LATTICE_LOGF("MESH", LogLevel::LOG_WARN, "Broadcast send failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void Mesh::debugDumpRadio() {
  if (!lattice::eeprom::getDevMode())
    return;
  uint8_t ch;
  esp_wifi_get_channel(&ch, nullptr);
  // Doesn't fit LATTICE_LOGF's single-fmt-string shape (variable-length hex dump
  // built in a loop), so it's gated by hand with the same #if LATTICE_LOGF uses —
  // under LATTICE_DEFAULT_LOG_LEVEL == LOG_NONE this whole block (buffer,
  // snprintf calls, format-string literals) compiles out entirely instead of
  // relying solely on the runtime getDevMode() check above (fix for the same
  // regression class as LATTICE_LOGF — item R / PR #86).
#if LATTICE_DEFAULT_LOG_LEVEL != LATTICE_LOG_LEVEL_NONE
  char buf[96];
  int off = snprintf(buf, sizeof(buf), "DBG Channel=%u Key=", (unsigned)ch);
  for (int i = 0; i < MESH_KEY_SIZE && off > 0 && off < (int)sizeof(buf); ++i) {
    off += snprintf(buf + off, sizeof(buf) - off, "%02X ", meshKey[i]);
  }
  LATTICE_LOGLN("MESH", buf, LogLevel::LOG_INFO);
#endif
}

void Mesh::checkMasterTimeout() {
  if (isMaster)
    return;
  if (currentMaster.distance == 0xFF)
    return; // No master known yet
  if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - lastMasterBeaconReceivedMs >
      STALE_MASTER_THRESHOLD_MS) {
    LATTICE_LOGLN("MESH", "Master beacon timeout — clearing route, treating as offline",
                  LogLevel::LOG_WARN);
    memset(currentMaster.mac, 0, 6);
    currentMaster.distance = 0xFF;
    memset(lastSeenMasterMac, 0, 6);
    lastMasterBeaconReceivedMs = 0;
  }
}

// ---------- Tiger Style helper implementations ----------

void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: ignore echoes of our own beacon relayed back by neighbours (relays are
  // broadcast, so the originating master hears them too). Without this the master
  // would TOFU-learn itself as knownMasterMac and record a bogus route to itself.
  if (lattice::mac::eq(msg.origin_mac_address, deviceMacAddress))
    return;

  // Master MAC pin (Phase D, #42): the beacon's origin_mac_address must match
  // the deployment-provisioned master MAC pinned at build time. Weaker
  // guarantee than the JOIN_ACK pubkey pin — WiFi MACs are trivially
  // spoofable, so this only rejects naive attackers, not a MAC-spoofing RF
  // attacker. Runs BEFORE any TOFU state mutation. DEV_MODE (compile-time)
  // bypasses this in dev firmware builds; the UNIT_TEST-only runtime bypass
  // lets tests toggle it without recompiling.
  if (!lattice::config::DEV_MODE && !lattice::mesh::pin::isTestBypassed()) {
    if (memcmp(msg.origin_mac_address, lattice::mesh::pin::MASTER_MAC,
               sizeof(lattice::mesh::pin::MASTER_MAC)) != 0) {
      LATTICE_LOGLN("MESH", "Beacon origin MAC mismatch pin — drop", LogLevel::LOG_ERROR);
      return;
    }
  }

  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // --- TOFU master MAC enforcement ---
  bool fromPrimary = enrollment.hasMasterMac &&
                     lattice::mac::eq(msg.origin_mac_address, enrollment.knownMasterMac);
  bool fromSecondary = _dualMasterMode && enrollment.hasMasterMacSecondary &&
                       lattice::mac::eq(msg.origin_mac_address, enrollment.knownMasterMacSecondary);

  if (!enrollment.hasMasterMac) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    memcpy(enrollment.knownMasterMac, msg.origin_mac_address, 6);
    enrollment.hasMasterMac = true;
    lattice::eeprom::saveKnownMasterMac(enrollment.knownMasterMac);
    LATTICE_LOGLN("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  } else if (!fromPrimary && !fromSecondary) {
    // Beacon from unrecognised MAC
    if (_dualMasterMode && !enrollment.hasMasterMacSecondary) {
      // Second master TOFU — learn and save as secondary
      memcpy(enrollment.knownMasterMacSecondary, msg.origin_mac_address, 6);
      enrollment.hasMasterMacSecondary = true;
      lattice::eeprom::saveKnownMasterMacSecondary(enrollment.knownMasterMacSecondary);
      LATTICE_LOGLN("MESH", "Secondary master MAC learned (TOFU)", LogLevel::LOG_INFO);
      // fall through to process this beacon as valid
    } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - lastMasterBeaconReceivedMs <
               STALE_MASTER_THRESHOLD_MS) {
      // Known master(s) still fresh — reject unknown MAC
      LATTICE_LOGLN("MESH", "Beacon from unexpected MAC rejected (master still alive)",
                    LogLevel::LOG_WARN);
      return;
    } else {
      // All known masters stale — accept as new primary (hotswap)
      LATTICE_LOGLN("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
      memcpy(enrollment.knownMasterMac, msg.origin_mac_address, 6);
      lattice::eeprom::saveKnownMasterMac(enrollment.knownMasterMac);
    }
  }

  if (!lattice::mac::eq(lastSeenMasterMac, msg.origin_mac_address) && lastSeenMasterMac[0] != 0) {
    if (_dualMasterMode) {
      LATTICE_LOGLN("MESH", "Two masters active (dual master mode)", LogLevel::LOG_DEBUG);
    } else {
      LATTICE_LOGLN("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
      lattice::err::fail(
          lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 7,
          "Multiple master nodes detected! Network split or misconfiguration likely.");
    }
  }
  memcpy(lastSeenMasterMac, msg.origin_mac_address, 6);
  lastMasterBeaconReceivedMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;

  // Multi-hop routing (spec §3): the node we heard this beacon THROUGH
  // (last_hop) is a forwarding candidate. msg.hop_count is last_hop's OWN
  // distance to the master (this receiving node's distance is one more, per
  // `newDistance` above — last_hop is one hop closer), so last_hop's distance
  // is msg.hop_count, not +1: a direct beacon straight from the master
  // (hop_count == 0, last_hop == master) must record the master itself as a
  // distance-0 neighbor. Learned here, not from enrollment — routing only.
  //
  // Derive currentMaster.distance from live NeighborTable state (issue #45) in
  // the SAME pass as the observe (post-Phase-G audit item X) — this used to be
  // neighbors.observe(...) followed by a separate neighbors.minFreshDistance(...)
  // call, two full linear scans of the neighbor table per beacon RX.
  // Sticky-min replaced by a pure function of neighbor state: rises monotonically
  // as shorter-path neighbors age out; no oscillation because state can only
  // flap if NeighborTable itself flaps.
  memcpy(currentMaster.mac, msg.origin_mac_address, 6);
  uint8_t min_d =
      neighbors.observeAndMinDistance(msg.last_hop_mac_address, msg.hop_count,
                                      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL);
  uint8_t derived = (min_d == 0xFF) ? 0xFF : static_cast<uint8_t>(min_d + 1);
  if (derived != currentMaster.distance) {
    currentMaster.distance = derived;
    LATTICE_LOGF("MESH", LogLevel::LOG_INFO, "Route distance derived: %u", (unsigned)derived);
  }

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    if (txState.wasRelayedBefore(msg.epoch_num, msg.seq_num)) {
      LATTICE_LOGLN("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    txState.markRelayed(msg.epoch_num, msg.seq_num);

    // Defer relay with random jitter to stagger transmissions across all non-master
    // nodes and eliminate the collision burst that occurs when all nodes relay
    // within milliseconds of receiving the same beacon.
    // Jitter window: 10–73 ms (10 + esp_random() % RELAY_JITTER_MAX_MS)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % lattice::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    // Relay carries this node's just-derived distance (`derived`, above), not
    // the naive msg.hop_count + 1 for this specific beacon's path — if a
    // shorter fresh neighbor already exists, downstream nodes should hear
    // this node's true (possibly smaller) distance, not an inflated one.
    relayPendingMsg.hop_count = derived;
    memcpy(relayPendingMsg.last_hop_mac_address, deviceMacAddress, 6);
    relayPendingAt = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL + 10 + jitterMs;
    relayPending = true;
  }
}

void Mesh::processAdapterData(const mesh_message& msg) {
  // OP_CONFIG_SET = 0xC1 (from lib/lattice-protocol/opcodes.h)
  bool addressedToSelf = (lattice::mac::eq(msg.target_mac_address, deviceMacAddress));
  bool isBroadcastTarget = (lattice::mac::eq(msg.target_mac_address, BROADCAST_MAC));
  bool addressedToMaster =
      enrollment.hasMasterMac && (lattice::mac::eq(msg.target_mac_address, currentMaster.mac));

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
    // Downlink toward a specific node. If the frame carries a source route and
    // we are on it, forward to the next hop (stateless — spec §4); otherwise
    // fall back to the flood.
    if (msg.route_len > 0 && msg.route_len <= lattice::config::MAX_HOPS) {
      for (uint8_t i = 0; i < msg.route_len; ++i) {
        if (lattice::mac::eq(&msg.route_path[static_cast<size_t>(i) * 6], deviceMacAddress)) {
          if (msg.hop_count >= lattice::config::MAX_HOPS)
            return;
          mesh_message fwd = msg;
          fwd.hop_count++;
          const uint8_t* next = (i + 1 < msg.route_len)
                                    ? &msg.route_path[static_cast<size_t>(i + 1) * 6]
                                    : msg.target_mac_address;
          // Bounded via the downlink forwarding-peer LRU (spec §2) — `next` is
          // attacker-controlled plaintext (this relay never opens the sealed
          // frame), so an unbounded registerPeerWithEspNow here would let an RF
          // attacker exhaust the ESP-NOW peer table one entry per crafted frame.
          registerDownlinkPeer(next);
          sendMessage(next, fwd);
          return;
        }
      }
    }
    relayDownlink(msg); // not on the route / no route -> existing flood fallback
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
    LATTICE_LOGLN("MESH",
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
      LATTICE_LOGLN("MESH", "E2E open failed — frame dropped", LogLevel::LOG_WARN);
      return;
    }
  }

  // Node-side E2E open (spec §2): a self-addressed sealed ADAPTER_DATA from the
  // master is opened with our k_down before local delivery. Mirrors the master's
  // uplink open above. Failure → drop (finding-#9 pattern). Broadcast (FF:FF)
  // frames are NOT opened — addressedToSelf is false for those, so this never
  // fires for them (they stay plaintext, handled below).
  bool nodeOpened = false;
  if (!isMaster && addressedToSelf && msg.message_type == MESH_TYPE_ADAPTER_DATA) {
    const uint8_t *kUp, *kDown;
    if (!masterE2EKeys(&kUp, &kDown) || !lattice::mesh::crypto::openPayload(kDown, opened)) {
      LATTICE_LOGLN("MESH", "downlink open failed — dropped", LogLevel::LOG_WARN);
      return;
    }
    nodeOpened = true;
  }

  bool isConfigOpcode = (opened.data_type == adapter_types::SERIAL_ADAPTER &&
                         (opened.data[0] == OP_CONFIG_SET || opened.data[0] == OP_NODE_ID_SET));
  // Critical fix: config opcodes (CONFIG_SET / NODE_ID_SET) are state-changing
  // (adapter-type reconfig + restart, node-identity assignment) and must be
  // honored ONLY via the sealed, opened path above (needsOpen on the master,
  // nodeOpened on a node) — never via a broadcast-target or otherwise-unopened
  // frame. Without this, a forged plaintext BROADCAST (target FF:FF)
  // ADAPTER_DATA frame is never addressedToSelf, so it is never opened; since
  // origin_mac is attacker-controlled and the master's real MAC is public in
  // beacons, such a frame sailed past the origin check below too and reached
  // externalRecvCallback fully unauthenticated (one plaintext RF frame could
  // reboot/reconfigure any node). Legitimate non-config broadcast adapter data
  // (e.g. OP_HEALTH_REQ, OP_TX_POWER_SET) is unaffected — this guard only
  // fires for CONFIG_SET/NODE_ID_SET.
  if (isConfigOpcode && !needsOpen && !nodeOpened) {
    LATTICE_LOGLN("MESH", "Config opcode via unopened/broadcast path rejected (unauthenticated)",
                  LogLevel::LOG_WARN);
    return;
  }
  if (isConfigOpcode && enrollment.hasMasterMac) {
    bool fromPrimary = lattice::mac::eq(opened.origin_mac_address, enrollment.knownMasterMac);
    bool fromSecondary =
        enrollment.hasMasterMacSecondary &&
        lattice::mac::eq(opened.origin_mac_address, enrollment.knownMasterMacSecondary);
    if (!fromPrimary && !fromSecondary) {
      LATTICE_LOGLN("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
      return;
    }
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
    if (lattice::mac::eq(peers.peerMacs[i].mac, deviceMacAddress))
      continue;
    sendMessage(peers.peerMacs[i].mac, relay);
  }
}

void Mesh::relayEnrollmentUplink(const mesh_message& msg) {
  // Never relay our own outbound request echoed back to us over the air.
  if (lattice::mac::eq(msg.origin_mac_address, deviceMacAddress))
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
  if (!lattice::mac::eq(msg.target_mac_address, deviceMacAddress)) {
    if (lattice::mac::eq(msg.origin_mac_address, deviceMacAddress))
      return;
    if (msg.hop_count >= lattice::config::MAX_HOPS)
      return;
    mesh_message relay = msg;
    relay.hop_count++;
    memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
    sendBroadcast(relay);
    return;
  }
  // Masters issue JOIN_ACKs; they never enroll via one. Without this guard a
  // forged ACK addressed to the master (fingerprint is observable over the
  // air) could TOFU-poison it and register attacker key material.
  if (isMaster) {
    LATTICE_LOGLN("MESH", "JOIN_ACK addressed to master — ignoring", LogLevel::LOG_WARN);
    return;
  }
  enrollment.processJoinAck(msg, deviceMacAddress, &Mesh::registerPeerWithKeyTrampoline);
}

bool Mesh::registerPeerWithKeyTrampoline(const uint8_t* mac, const uint8_t* publicKey32) {
  return instance->registerPeerWithKey(mac, publicKey32, /*allowRekey=*/false);
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
      // Thinned via lattice::mem::is_zero (Phase H2 audit item Z).
      bool keyEstablished = !lattice::mem::is_zero(p->publicKey, 32);
      if (keyEstablished) {
        LATTICE_LOGLN("MESH", "Peer already registered — keeping established key",
                      LogLevel::LOG_DEBUG);
        p->lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        return true; // already routable; nothing to change
      }
    }
    // Update existing peer's public key
    memcpy(p->publicKey, publicKey32, 32);
    p->lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  } else {
    if (peers.peerCount >= MAX_PEERS) {
      LATTICE_LOGLN("MESH", "Peer list full, cannot enroll", LogLevel::LOG_WARN);
      return false;
    }
    PeerInfo newPeer;
    memcpy(newPeer.mac, mac, 6);
    memcpy(newPeer.publicKey, publicKey32, 32);
    newPeer.lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    peers.append(newPeer);
  }
  peers.saveToEEPROM();

  // Re-register with encryption now that we have the public key (mbedtls-heavy via Enrollment)
  enrollment.enrollPeer(mac, publicKey32, nullptr, _dualMasterMode);
  return true;
}

void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32) {
  enrollPeer(mac, publicKey32, nullptr, nullptr);
}

void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                      const uint8_t* secondaryPubKey32) {
  if (!registerPeerWithKey(mac, publicKey32, /*allowRekey=*/true))
    return; // registry full — do not ACK an enrollment we could not record

  // Send JOIN_ACK unicast to new node
  mesh_message ack = {};
  // Stamp proto_version + (epoch, seq) so the existing ReplayCache dedups
  // re-broadcast copies of this ACK (Task 9c R2): each relay node re-broadcasts a
  // given JOIN_ACK at most once (the reflected copy is dropped by isReplay before
  // processJoinAck), preventing combinatorial broadcast amplification.
  ack.proto_version = PROTO_VERSION;
  // Draw seq via the guarded choke point FIRST — it may bump txState.bootEpoch
  // on wrap — then stamp epoch_num from the (possibly just-bumped) value so
  // the ACK's epoch always matches the epoch its seq_num was drawn under.
  ack.seq_num = nextSeqGuarded();
  ack.epoch_num = txState.bootEpoch;
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
  // Stamp the server-provided secondary-master identity, if any, so the
  // enrolling node can TOFU-learn its failover master from this same ACK
  // (Phase 4). Protocol v0.6.0 (wire shrink §8) packs this into the JOIN_ACK
  // data[] payload rather than top-level MeshMessage fields:
  //   data[0..4]   = node pubkey fingerprint (set above)
  //   data[4..10]  = secondaryMasterMac
  //   data[10..42] = secondaryPublicKey
  //   data[42..64] = zero
  // Left zeroed (ack's default) when there is no secondary.
  if (secondaryMac && secondaryPubKey32) {
    memcpy(ack.data + 4, secondaryMac, 6);
    memcpy(ack.data + 10, secondaryPubKey32, 32);
  }
  // Broadcast via the registered FF:FF:… peer so the new node receives the ACK
  // even before it is individually registered as a unicast peer.
  sendBroadcast(ack);
  LATTICE_LOGLN("MESH", "JOIN_ACK sent to newly enrolled node", LogLevel::LOG_INFO);
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
      LATTICE_LOGLN("MESH", "E2E open failed — route report dropped", LogLevel::LOG_WARN);
      return;
    }
    if (opened.data[0] != OP_ROUTE_REPORT) {
      LATTICE_LOGLN("MESH", "processRouteReport: bad opcode, dropping", LogLevel::LOG_WARN);
      return;
    }
    if (msg.route_len > lattice::config::MAX_HOPS) {
      // Tiger-Style: bounds-check before indexing route_path below — a
      // corrupt/hostile route_len must never drive an out-of-bounds read.
      LATTICE_LOGLN("MESH", "Route report: route_len exceeds MAX_HOPS, dropping",
                    LogLevel::LOG_ERROR);
      return;
    }

    // Chain-MAC verify (Phase C, spec §4 / issue #44): reconstruct the same
    // per-hop HMAC chain the origin seeded and each relay extended, keyed
    // off each hop's own pairwise k_up with this master, and compare against
    // msg.auth_path. route_path never records the origin's own MAC (only
    // relay-appended hops — see RelayAppendsOwnMacToRoutePath), so hop 0 is
    // always the origin itself; kUp (derived above for the E2E open) is
    // reused immediately here rather than re-derived, since a subsequent
    // getKeys() call below (for a different peer) can evict/invalidate it
    // (E2EKeyStore.h — "must use immediately, not cache across calls").
    uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
    uint8_t prev_hop[6] = {0};
    {
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, msg.origin_mac_address, ctx);
      routemac::chainStep(kUp, ctx, computed, computed);
      memcpy(prev_hop, msg.origin_mac_address, 6);
    }
    for (uint8_t i = 0; i < msg.route_len; ++i) {
      const uint8_t* hop_mac = &msg.route_path[static_cast<size_t>(i) * 6];
      const uint8_t *hopKUp, *hopKDown;
      if (!peerE2EKeys(hop_mac, &hopKUp, &hopKDown)) {
        LATTICE_LOGLN("MESH", "Route report: unknown hop, dropping", LogLevel::LOG_ERROR);
        return;
      }
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, hop_mac, ctx);
      routemac::chainStep(hopKUp, ctx, computed, computed);
      memcpy(prev_hop, hop_mac, 6);
    }
    if (memcmp(computed, msg.auth_path, routemac::AUTH_PATH_LEN) != 0) {
      LATTICE_LOGLN("MESH", "Route report: MAC verify failed, dropping", LogLevel::LOG_ERROR);
      return;
    }

    // Learn the origin's relay path for downlink source routing (spec §4).
    // route_path/route_len are plaintext header fields (accumulated by relays);
    // bounds-checked by RouteTable::record. Only recorded on MAC-verify pass.
    if (routes) {
      routes->record(msg.origin_mac_address, msg.route_path, msg.route_len,
                     static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL);
    }
    // Terminal endpoint — deliver to server via external callback
    if (externalRecvCallback)
      externalRecvCallback(opened);
    return;
  }

  // Relay node (spec §4): the payload is E2E-sealed origin->master and opaque to
  // us. Accumulate the relay path in the plaintext route_path header (excluded
  // from AAD, so this does not break the tag) so the master learns the full
  // origin->master relay chain for downlink source routing.
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "processRouteReport: hop limit reached, dropping", LogLevel::LOG_WARN);
    return;
  }
  if (msg.route_len >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "route report path full — dropping", LogLevel::LOG_WARN);
    return;
  }

  // Chain-MAC extend (Phase C, spec §4 / issue #44): snapshot the previous
  // last hop BEFORE appending this relay's own MAC to route_path. route_path
  // never records the origin's own MAC (only relay-appended hops — see
  // RelayAppendsOwnMacToRoutePath), so when this is the first relay
  // (route_len == 0) the "previous hop" is the origin itself, not a
  // route_path entry. Reads from msg (pre-copy) — identical to relay at this
  // point, but relay isn't constructed until the next line.
  uint8_t prev_hop[6];
  if (msg.route_len == 0) {
    memcpy(prev_hop, msg.origin_mac_address, 6);
  } else {
    memcpy(prev_hop, &msg.route_path[static_cast<size_t>(msg.route_len - 1) * 6], 6);
  }

  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  memcpy(&relay.route_path[static_cast<size_t>(relay.route_len) * 6], deviceMacAddress, 6);
  relay.route_len++;

  // Fold this relay's hop into msg.auth_path, keyed off its own pairwise
  // k_up with the master — the same key material the E2E seal path already
  // relies on (masterE2EKeys), so no new provisioning is needed. If the
  // master isn't known yet (rare — relay would also fail the routing lookup
  // above), drop and log rather than forwarding an unauthenticated hop.
  const uint8_t *kUp, *kDown;
  if (!masterE2EKeys(&kUp, &kDown)) {
    LATTICE_LOGLN("MESH", "Route report: no k_up for master, dropping relay hop",
                  LogLevel::LOG_WARN);
    return;
  }
  uint8_t ctx[routemac::HOP_CTX_LEN];
  routemac::buildHopContext(relay, prev_hop, deviceMacAddress, ctx);
  routemac::chainStep(kUp, ctx, relay.auth_path, relay.auth_path);

  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ROUTE_REPORT,
               &relay);
}

void Mesh::loop() {
  // Phase I Task 9 (item EE): recvQueue draining moved off loop() and onto
  // the dedicated mesh task (main.cpp's mesh_task_fn), woken via
  // xTaskNotifyWait() when onDataRecvCallback's ISR trampoline signals
  // drainNotifyHandle_ — see drain()/setDrainNotifyHandle() in Mesh.h. Host
  // unit tests and the SimNode e2e harness (no real FreeRTOS task) call
  // drain() directly instead.
  lattice::eeprom::flushIfDirty();

  // Drain enrollment relay queued from ESP-NOW receive callback (WiFi task context).
  // Serial.write() must not be called from that callback — safe to do here in loop().
  enrollment.drainPendingRelay();

  {
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
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
  if (relayPending && static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL >= relayPendingAt) {
    relayPending = false;
    sendBroadcast(relayPendingMsg);
  }

  // Master beacon — broadcastMasterBeacon() guards timing internally via lastBeaconMillis
  if (isMaster) {
    broadcastMasterBeacon();
  }
}

} // namespace mesh
} // namespace lattice
