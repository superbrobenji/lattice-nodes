#include "Mesh.h"
#include "src/network/MacEq.h"
#include "src/network/hw_mac.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h" // unified error
#include "src/persistence/eeprom/EepromCore.h"
#include "src/persistence/eeprom/EepromDiagnostics.h"
#include "src/persistence/eeprom/EepromDeviceConfig.h"
#include "src/persistence/eeprom/EepromSecurity.h"
// Error.h already provides ERROR_CHECK macros
#include <esp_now.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include "../../project_config.h"
#include "broadcast_mac.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

Mesh* Mesh::instance = nullptr;

Mesh::Mesh()
    : isMaster(false), relayPendingAt(0), relayPending(false),
      _dualMasterMode(lattice::config::DUAL_MASTER_MODE), lastBeaconMs(0), lastRouteReportMs(0) {
  instance = this;
  memset(currentMaster.mac, 0, 6);
  currentMaster.distance = 0xFF;
  memset(lastSeenMasterMac, 0, 6);
  memset(deviceMacAddress, 0, 6);
  memset(&relayPendingMsg, 0, sizeof(relayPendingMsg));
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
  if (!setupRadio())
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
  if (!transport.setupEspNow(meshKey, peers))
    return false;

  return true;
}

bool Mesh::setupRadio() {
  // Wi-Fi bring-up moved to MeshTransport::setup() (Phase B Task 4). MAC
  // address ownership stays here — readMacAddress()/peers.setDeviceMac()
  // need deviceMacAddress, which many other Mesh methods also depend on.
  if (!transport.setup())
    return false;
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

// ------------------------------------------------

// Dispatch for one message dequeued by transport.drain() (Phase B Task 4).
// Reached via handleReceivedMessageTrampoline. Body is the old
// Mesh::drainRecvQueue's post-pop logic, unchanged: proto-version check,
// replay check, peers.updateLastSeen, then the message-type switch into
// Mesh-owned handlers.
void Mesh::handleReceivedMessage(const uint8_t* srcMac, const mesh_message& msg) {
  // Proto version check: drop anything that isn't exactly the current wire
  // version. There is no legitimate proto_version==0 case — buildMessage(),
  // Enrollment::sendRequest(), and the JOIN_ACK path all stamp PROTO_VERSION
  // unconditionally — so a zero value only ever means a forged/malformed
  // frame that would otherwise bypass both this flag-day drop and the replay
  // gate below (which is itself keyed on proto_version == PROTO_VERSION).
  if (msg.proto_version != PROTO_VERSION) {
    LATTICE_LOGLN("MESH", "Unsupported proto version, dropping", LogLevel::LOG_WARN);
    return;
  }

  // Replay check
  if (msg.proto_version == PROTO_VERSION && msg.epoch_num > 0) {
    if (replay.isReplay(msg, static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL)) {
      LATTICE_LOGLN("MESH", "Replayed message dropped", LogLevel::LOG_DEBUG);
      return;
    }
  }

  // Update last-seen for known peers only (no EEPROM write — see Task 4)
  peers.updateLastSeen(srcMac);

  switch (msg.message_type) {
  case MESH_TYPE_ENROLLMENT:
    if (isMaster) {
      // Cache the enrolling node's real Curve25519 public key now, while we
      // have it (this is the only point the master ever sees it directly —
      // the server's later JOIN_ACK response echoes back only a 4-byte
      // fingerprint plus this master's OWN public key, never the node's).
      // Without this, MeshMessenger::enrollPeer had nothing correct to build
      // the outbound JOIN_ACK's fingerprint from, so every node's
      // Enrollment::processJoinAck fingerprint check failed permanently —
      // no leaf could ever complete enrollment (lattice-hub#178).
      // allowRekey=false: a later forged ENROLLMENT for the same MAC must
      // not override the first key seen.
      lattice::mesh::registerPeerWithKey(msg.origin_mac_address, msg.enrollment_public_key,
                                         /*allowRekey=*/false, peers, enrollment,
                                         _dualMasterMode);
      enrollment.processRequest(msg);
    } else
      messenger.relayEnrollmentUplink(msg, deviceMacAddress, currentMaster, txState, peers,
                                      enrollment, e2eKeys, uplinkRouter, neighbors, transport);
    break;
  case MESH_TYPE_JOIN_ACK:
    processJoinAck(msg);
    break;
  case MESH_TYPE_MASTER_BEACON:
    beacon.process(msg, deviceMacAddress, isMaster, _dualMasterMode, enrollment, neighbors,
                   currentMaster, txState, relayPendingMsg, relayPendingAt, relayPending,
                   lastSeenMasterMac);
    break;
  case MESH_TYPE_ADAPTER_DATA:
    processAdapterData(msg);
    break;
  case MESH_TYPE_ROUTE_REPORT:
    routeReportHandler.processRouteReport(msg, isMaster, peers, enrollment, e2eKeys, routes.get(),
                                          deviceMacAddress, currentMaster, txState, messenger,
                                          uplinkRouter, neighbors, transport, externalRecvCallback);
    break;
  default:
    LATTICE_LOGLN("MESH", "Unknown message type, dropping", LogLevel::LOG_WARN);
  }
}

// Static trampoline through the singleton `instance` (Adapter holds a plain
// function-pointer member — see this method's declaration comment in Mesh.h
// for why it can't be a capturing lambda/std::function). Body moved to
// MeshMessenger::transmitDispatch (round 2 task 11) — this just threads every
// collaborator a send needs through the call.
void Mesh::transmit(const adapter_types type, const uint8_t* data) {
  if (!instance) {
    LATTICE_LOGLN("MESH", "transmit() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->messenger.transmitDispatch(
      type, data, /*selfOriginated=*/false, instance->isMaster, instance->externalRecvCallback,
      instance->deviceMacAddress, instance->currentMaster, instance->txState, instance->peers,
      instance->enrollment, instance->e2eKeys, instance->uplinkRouter, instance->neighbors,
      instance->transport);
}

void Mesh::transmitSelfOriginated(const adapter_types type, const uint8_t* data) {
  if (!instance) {
    LATTICE_LOGLN("MESH", "transmitSelfOriginated() called before init", LogLevel::LOG_WARN);
    return;
  }
  instance->messenger.transmitDispatch(
      type, data, /*selfOriginated=*/true, instance->isMaster, instance->externalRecvCallback,
      instance->deviceMacAddress, instance->currentMaster, instance->txState, instance->peers,
      instance->enrollment, instance->e2eKeys, instance->uplinkRouter, instance->neighbors,
      instance->transport);
}

void Mesh::linkDataRecvCallback(std::function<void(const mesh_message&)> recvCallback) {
  externalRecvCallback = recvCallback;
}

// --- Periodically called in main loop if this node is master ---
// Thin wrapper (Phase B Task 5): timing + duplicate-send suppression now live
// on beacon (MasterBeacon::intervalElapsed), the actual send on
// MasterBeacon::send — buildMessage (crypto/sequencing) now lives on
// messenger (round 2 task 11).
void Mesh::broadcastMasterBeacon() {
  if (!beacon.intervalElapsed())
    return;
  mesh_message msg =
      messenger.buildMessage(adapter_types::UNKNOWN_ADAPTER, nullptr, MESH_TYPE_MASTER_BEACON,
                             deviceMacAddress, currentMaster, txState);
  msg.data[0] = 1; // protocolVersion
  msg.hop_count = 0;
  beacon.send(msg, transport);
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
    // Will be skipped automatically in dev mode (lattice::eeprom::saveMeshKey no-ops there).
    lattice::eeprom::saveMeshKey(meshKey, MESH_KEY_SIZE);
  }
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

// Thin wrapper (Phase B Task 5) — body moved to MasterBeacon::checkTimeout.
void Mesh::checkMasterTimeout() {
  beacon.checkTimeout(isMaster, currentMaster, lastSeenMasterMac);
}

// Phase C Task 2: moved verbatim from main.cpp's housekeeping_task_fn inline
// enrollment state machine — Mesh already owns isEnrolled()/getIsMaster()/
// sendEnrollmentRequest(), the state this decision is based on.
bool Mesh::tickEnrollmentBroadcast(uint64_t nowMs) {
  if (isEnrolled() || getIsMaster()) {
    return false;
  }
  if (nowMs - lastEnrollmentBroadcastMs_ > 10000) {
    lastEnrollmentBroadcastMs_ = nowMs;
    sendEnrollmentRequest();
    Logger::logln("MAIN", "Enrollment request sent (awaiting server approval)", LogLevel::LOG_INFO);
  }
  return true;
}

// ---------- Tiger Style helper implementations ----------

void Mesh::processAdapterData(const mesh_message& msg) {
  bool addressedToSelf = (lattice::mac::eq(msg.target_mac_address, deviceMacAddress));
  bool isBroadcastTarget = (lattice::mac::eq(msg.target_mac_address, BROADCAST_MAC));
  bool addressedToMaster =
      enrollment.hasMasterMac && (lattice::mac::eq(msg.target_mac_address, currentMaster.mac));

  // Downlink/uplink routing decision (Phase B Task 6, finding 1 job 4
  // narrowed; finding 2's routing half) — delegated to DownlinkRouter, which
  // is crypto-free and read-only here. Every check below is unchanged from
  // the original inline block; only which class owns the classification
  // logic changed. The relay-toward-master case still executes here (not in
  // DownlinkRouter) because transmitCore needs lattice::mesh::masterE2EKeys
  // (E2EKeyLookup.h) and txState.checkEpochRollback.
  uint8_t nextHop[6];
  switch (router.classify(msg, deviceMacAddress, isMaster, addressedToSelf, isBroadcastTarget,
                          addressedToMaster, nextHop)) {
  case RouteDecision::DropHopLimitExceeded:
    // Matches the original's unconditional `return;` on hop-limit-exceeded —
    // drop the frame outright, do NOT fall through to the security gate
    // below (see DownlinkRouter.h's classify() doc comment).
    return;
  case RouteDecision::RelayTowardMaster: {
    // Uplink: relay toward master via routing table
    mesh_message relay = msg;
    relay.hop_count++;
    memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
    messenger.transmitCore(static_cast<adapter_types>(relay.data_type), relay.data,
                           MESH_TYPE_ADAPTER_DATA, &relay, isMaster, deviceMacAddress,
                           currentMaster, txState, peers, enrollment, e2eKeys, uplinkRouter,
                           neighbors, transport);
    return;
  }
  case RouteDecision::ForwardOnRoute: {
    // Downlink toward a specific node, on this node's source route — forward
    // to the next hop (stateless — spec §4).
    mesh_message fwd = msg;
    fwd.hop_count++;
    // Bounded via the downlink forwarding-peer LRU (spec §2) — `nextHop` is
    // attacker-controlled plaintext (this relay never opens the sealed
    // frame), so an unbounded registerPeerWithEspNow here would let an RF
    // attacker exhaust the ESP-NOW peer table one entry per crafted frame.
    router.registerDownlinkPeer(nextHop, peers, currentMaster);
    transport.sendMessage(nextHop, fwd, deviceMacAddress);
    return;
  }
  case RouteDecision::Flood:
    router.relayDownlink(msg, peers, deviceMacAddress,
                         transport); // not on the route / no route -> existing flood fallback
    return;
  case RouteDecision::NotRouted:
    break; // fall through to the security gate below, unchanged
  }

  // Authorization decision + E2E open (round 2 task 13) — delegated to
  // FrameAuthorizer, which owns the master-not-self-addressed sealed-type gate,
  // both E2E-open branches, and both config-opcode gates verbatim from what used
  // to be this function's own body. See FrameAuthorizer.h/.cpp for the full
  // security rationale (including the forged-broadcast-config-opcode attack the
  // gates below close) — Mesh keeps only local-delivery dispatch after this call.
  mesh_message opened{};
  if (frameAuthorizer.authorize(msg, isMaster, addressedToSelf, currentMaster, peers, enrollment,
                                e2eKeys, opened) == AuthResult::Rejected) {
    return;
  }
  if (externalRecvCallback)
    externalRecvCallback(opened);

  // Broadcast: also relay so multi-hop nodes receive it (Task 3 test covers this)
  if (isBroadcastTarget && !isMaster) {
    router.relayDownlink(msg, peers, deviceMacAddress, transport);
  }
}

// Outer JOIN_ACK dispatch moved to lattice::mesh::dispatchJoinAck (round 2 task
// 10, PeerEnrollment.h/.cpp) — verbatim, see that function's doc comment for
// the relay-vs-process reasoning this wrapper used to spell out inline.
void Mesh::processJoinAck(const mesh_message& msg) {
  lattice::mesh::dispatchJoinAck(msg, deviceMacAddress, isMaster, enrollment,
                                 &Mesh::registerPeerWithKeyTrampoline);
}
// --------------------------------------------------------

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
      if (routeReportHandler.sendRouteReport(isMaster, uplinkRouter, currentMaster, peers,
                                             neighbors, enrollment, e2eKeys, deviceMacAddress,
                                             txState, messenger, transport))
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
    transport.sendBroadcast(relayPendingMsg);
  }

  // Master beacon — broadcastMasterBeacon() guards timing internally via beacon.intervalElapsed()
  if (isMaster) {
    broadcastMasterBeacon();
  }
}

} // namespace mesh
} // namespace lattice
