// mesh_logic_impl.cpp — real implementations of isReplay() and processMasterBeacon()
// compiled into test executables alongside firmware_stubs.cpp.
// UNIT_TEST is defined globally via add_compile_definitions(UNIT_TEST) in CMakeLists.txt.
// All mbedtls-using methods remain in firmware_stubs.cpp as stubs.

#include "Arduino.h"
#include "esp_now.h"
#include "WiFi.h"
#include "Mesh/Mesh.h"
#include "src/network/MacAddress.h"
#include "src/logging/Logger.h"
#include "src/persistence/EEPROM_Manager.h"
#include "../../main/project_config.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;

bool Mesh::isReplay(const mesh_message& msg) {
  for (size_t i = 0; i < REPLAY_CACHE_SIZE; ++i) {
    if (memcmp(replayCache[i].mac, msg.origin_mac_address, 6) == 0 &&
        replayCache[i].epoch == msg.epoch_num && replayCache[i].seq == msg.seq_num) {
      return true;
    }
  }
  // Record this entry in the ring buffer
  memcpy(replayCache[replayCacheIdx].mac, msg.origin_mac_address, 6);
  replayCache[replayCacheIdx].epoch = msg.epoch_num;
  replayCache[replayCacheIdx].seq = msg.seq_num;
  replayCacheIdx = (replayCacheIdx + 1) % REPLAY_CACHE_SIZE;
  return false;
}

void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    Logger::logln("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // --- TOFU master MAC enforcement ---
  bool fromPrimary = hasMasterMac && memcmp(msg.origin_mac_address, knownMasterMac, 6) == 0;
  bool fromSecondary = _dualMasterMode && hasMasterMacSecondary &&
                       memcmp(msg.origin_mac_address, knownMasterMacSecondary, 6) == 0;

  if (!hasMasterMac) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    memcpy(knownMasterMac, msg.origin_mac_address, 6);
    hasMasterMac = true;
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  } else if (!fromPrimary && !fromSecondary) {
    // Beacon from unrecognised MAC
    if (_dualMasterMode && !hasMasterMacSecondary) {
      // Second master TOFU — learn and save as secondary
      memcpy(knownMasterMacSecondary, msg.origin_mac_address, 6);
      hasMasterMacSecondary = true;
      EEPROM_Manager::getInstance().saveKnownMasterMacSecondary(knownMasterMacSecondary);
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
      memcpy(knownMasterMac, msg.origin_mac_address, 6);
      EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    }
  }

  if (lattice::utils::MacAddress(lastSeenMasterMac) !=
          lattice::utils::MacAddress(msg.origin_mac_address) &&
      lastSeenMasterMac[0] != 0) {
    if (_dualMasterMode) {
      Logger::logln("MESH", "Two masters active (dual master mode)", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
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
  }

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    bool isNewer = (msg.epoch_num > lastRelayedEpoch) ||
                   (msg.epoch_num == lastRelayedEpoch && msg.seq_num > lastRelayedSeqNum);
    if (!isNewer) {
      Logger::logln("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    lastRelayedEpoch = msg.epoch_num;
    lastRelayedSeqNum = msg.seq_num;

    // Defer relay with jitter (esp_random() returns deterministic 42 in tests)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % lattice::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hop_count = newDistance;
    memcpy(relayPendingMsg.last_hop_mac_address, deviceMacAddress, 6);
    relayPendingAt = millis() + 10 + jitterMs;
    relayPending = true;
  }
}

void Mesh::drainRecvQueue() {
  while (recvQueueTail != recvQueueHead) {
    RecvQueueEntry& entry = recvQueue[recvQueueTail];
    recvQueueTail = (recvQueueTail + 1) % RECV_QUEUE_SIZE;

    const mesh_message& msg = entry.msg;

    // Proto version check
    if (msg.proto_version != 0 && msg.proto_version != PROTO_VERSION) {
      Logger::logln("MESH", "Unsupported proto version, dropping", LogLevel::LOG_WARN);
      continue;
    }

    // Replay check
    if (msg.proto_version == PROTO_VERSION && msg.epoch_num > 0) {
      if (isReplay(msg)) {
        Logger::logln("MESH", "Replayed message dropped", LogLevel::LOG_DEBUG);
        continue;
      }
    }

    // Update last-seen for known peers only
    updatePeerLastSeen(entry.srcMac);

    switch (msg.message_type) {
    case MESH_TYPE_ENROLLMENT:
      processEnrollmentRequest(msg);
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

bool Mesh::appendPeer(const PeerInfo& peer) {
  if (peerCount >= MAX_PEERS)
    return false;
  peerMacs[peerCount++] = peer;
  return true;
}

void Mesh::sendMessage(const uint8_t target[6], mesh_message msg) {
  if (lattice::utils::MacAddress(target) == lattice::utils::MacAddress(deviceMacAddress)) {
    Logger::logln("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  esp_err_t result = esp_now_send(target, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (result == ESP_OK) {
    Logger::logln("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("MESH", "Error sending message", LogLevel::LOG_WARN);
  }
}

void Mesh::relayDownlink(const mesh_message& msg) {
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue;
    sendMessage(peerMacs[i].mac, relay);
  }
}

void Mesh::processJoinAck(const mesh_message& msg) {
  // Relay outward if not addressed to us (multi-hop enrollment)
  if (memcmp(msg.target_mac_address, deviceMacAddress, 6) != 0) {
    relayDownlink(msg);
    return;
  }
  // Verify fingerprint matches our public key (first 4 bytes)
  if (memcmp(msg.data, devicePublicKey, 4) != 0) {
    Logger::logln("MESH", "JOIN_ACK fingerprint mismatch — ignoring", LogLevel::LOG_WARN);
    return;
  }
  Logger::logln("MESH", "Enrollment approved! Saving enrolled flag.", LogLevel::LOG_INFO);
  EEPROM_Manager::getInstance().saveEnrolledFlag(true);

  // The node sending JOIN_ACK is the master — record its MAC (TOFU)
  if (!hasMasterMac) {
    memcpy(knownMasterMac, msg.origin_mac_address, 6);
    hasMasterMac = true;
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned and saved (TOFU)", LogLevel::LOG_INFO);
  }
}

void Mesh::transmitCore(const adapter_types type, const uint8_t data[64], MeshMessageType msgType,
                        const mesh_message* msgOverride) {
  mesh_message msg;
  if (msgOverride) {
    msg = *msgOverride;
  } else {
    msg = buildMessage(type, data, msgType);
  }

  // Only for adapter data, set target as master
  if (msgType == MESH_TYPE_ADAPTER_DATA) {
    memcpy(msg.target_mac_address, currentMaster.mac, 6);
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop = findNextHopToMaster();
  if (nextHop && lattice::utils::MacAddress(nextHop->mac) !=
                     lattice::utils::MacAddress(deviceMacAddress)) {
    sendMessage(nextHop->mac, msg);
  } else {
    // Intentionally stubbed: production calls err::fail(); tests never hit this
    // path because fixtures always register master as a live peer.
    Logger::logln("MESH", "No next hop to master", LogLevel::LOG_WARN);
  }
}

PeerInfo* Mesh::findPeer(const uint8_t mac[6]) {
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, mac, 6) == 0) {
      return &peerMacs[i];
    }
  }
  return nullptr;
}

bool Mesh::isPeerInRange(const uint8_t mac[6]) {
  PeerInfo* peer = findPeer(mac);
  if (!peer)
    return false;
  return millis() - peer->lastSeenMillis < lattice::config::STALE_PEER_THRESHOLD_MS;
}

PeerInfo* Mesh::findNextHopToMaster() {
  // For this mesh: nextHop == currentMaster.nextHop
  if (currentMaster.distance == 0xFF)
    return nullptr;
  for (size_t i = 0; i < peerCount; ++i) {
    if (lattice::utils::MacAddress(peerMacs[i].mac) ==
            lattice::utils::MacAddress(currentMaster.nextHop) &&
        isPeerInRange(peerMacs[i].mac) &&
        lattice::utils::MacAddress(peerMacs[i].mac) !=
            lattice::utils::MacAddress(deviceMacAddress))
      return &peerMacs[i];
  }
  return nullptr;
}

void Mesh::broadcastToAllPeers(mesh_message msg) {
  if (peerCount == 0) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue; // Skip self
    sendMessage(peerMacs[i].mac, msg);
  }
}

void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[64]) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memset(msg.target_mac_address, 0xFF, 6); // broadcast indicator — relayed by all intermediate nodes
  broadcastToAllPeers(msg);
}

bool Mesh::sendRouteReport() {
  if (isMaster) return false;
  PeerInfo* nextHop = findNextHopToMaster();
  if (!nextHop) return false;

  mesh_message msg = {};
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.origin_mac_address, deviceMacAddress, 6);
  memcpy(msg.target_mac_address, currentMaster.mac, 6);
  memcpy(msg.last_hop_mac_address, deviceMacAddress, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 0; // path_len — incremented by each relay hop

  sendMessage(nextHop->mac, msg);
  return true;
}

void Mesh::processRouteReport(const mesh_message& msg) {
  // Verify opcode
  if (msg.data[0] != OP_ROUTE_REPORT) {
    Logger::logln("MESH", "processRouteReport: bad opcode, dropping", LogLevel::LOG_WARN);
    return;
  }

  if (isMaster) {
    // Terminal endpoint — deliver to server via external callback
    if (externalRecvCallback) externalRecvCallback(msg);
    return;
  }

  // Relay node: append own MAC to path and forward toward master
  uint8_t path_len = msg.data[1];
  if (path_len >= lattice::config::MAX_ROUTE_PATH_LEN) {
    Logger::logln("MESH", "processRouteReport: path full, dropping", LogLevel::LOG_WARN);
    return;
  }

  mesh_message relay = msg;
  memcpy(&relay.data[2 + path_len * 6], deviceMacAddress, 6);
  relay.data[1]++;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);

  if (relay.hop_count >= lattice::config::MAX_HOPS) {
    Logger::logln("MESH", "processRouteReport: hop limit reached, dropping", LogLevel::LOG_WARN);
    return;
  }

  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ROUTE_REPORT,
               &relay);
}

void Mesh::linkDataRecvCallback(std::function<void(const mesh_message&)> recvCallback) {
  externalRecvCallback = recvCallback;
}

void Mesh::processAdapterData(const mesh_message& msg) {
  // OP_CONFIG_SET = 0xC1 (from main/lib/lattice-protocol/opcodes.h)
  static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  bool addressedToSelf = (memcmp(msg.target_mac_address, deviceMacAddress, 6) == 0);
  bool isBroadcastTarget = (memcmp(msg.target_mac_address, kBroadcastMac, 6) == 0);
  bool addressedToMaster =
      hasMasterMac && (memcmp(msg.target_mac_address, currentMaster.mac, 6) == 0);

  if (!isMaster && !addressedToSelf && !isBroadcastTarget) {
    if (addressedToMaster) {
      // Uplink: relay toward master via routing table
      if (msg.hop_count >= lattice::config::MAX_HOPS)
        return;
      mesh_message relay = msg;
      relay.hop_count++;
      memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
      transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ADAPTER_DATA, &relay);
      return;
    }
    // Downlink to another node: relay outward toward specific target
    relayDownlink(msg);
    return;
  }

  // Local delivery
  bool isConfigOpcode =
      (msg.data_type == adapter_types::SERIAL_ADAPTER && msg.data[0] == OP_CONFIG_SET);
  // TODO(dual-master): also allow secondary master MAC for CONFIG_SET
  if (isConfigOpcode && hasMasterMac && memcmp(msg.origin_mac_address, knownMasterMac, 6) != 0) {
    Logger::logln("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
    return;
  }
  // Warn if master receives adapter data not addressed to itself — unexpected topology
  if (isMaster && !addressedToSelf && !isBroadcastTarget) {
    Logger::logln("MESH", "Master received ADAPTER_DATA not addressed to self", LogLevel::LOG_WARN);
  }
  if (externalRecvCallback)
    externalRecvCallback(msg);

  // Broadcast: also relay so multi-hop nodes receive it (Task 3 test covers this)
  if (isBroadcastTarget && !isMaster) {
    relayDownlink(msg);
  }
}

void Mesh::sendEnrollmentRequest() {
  mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  msg.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.origin_mac_address, deviceMacAddress, 6);
  memset(msg.target_mac_address, 0xFF, 6);
  memcpy(msg.last_hop_mac_address, deviceMacAddress, 6);
  msg.hop_count = 0;
  memcpy(msg.enrollment_public_key, devicePublicKey, 32);

  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  Logger::logln("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}

void Mesh::processEnrollmentRequest(const mesh_message& msg) {
  if (!isMaster) {
    return;
  }
  memcpy(_pendingEnrollmentMac, msg.origin_mac_address, 6);
  memcpy(_pendingEnrollmentPubKey, msg.enrollment_public_key, 32);
  _pendingEnrollmentRelay = true;
  Logger::logln("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}

void Mesh::setEnrollmentRelayFn(EnrollmentRelayFn fn) {
  _enrollmentRelayFn = fn;
}

void Mesh::drainPendingEnrollment() {
  if (!_pendingEnrollmentRelay)
    return;
  _pendingEnrollmentRelay = false;
  if (_enrollmentRelayFn) {
    _enrollmentRelayFn(_pendingEnrollmentMac, _pendingEnrollmentPubKey);
  }
}

} // namespace mesh
} // namespace lattice
