// mesh_logic_impl.cpp — real implementations of isReplay() and processMasterBeacon()
// compiled into test executables alongside firmware_stubs.cpp.
// UNIT_TEST is defined globally via add_compile_definitions(UNIT_TEST) in CMakeLists.txt.
// All mbedtls-using methods remain in firmware_stubs.cpp as stubs.

#include "Arduino.h"
#include "esp_now.h"
#include "WiFi.h"
#include "Mesh/Mesh.h"
#include "src/network/MacAddress.h"
#include "src/core/Logger.h"
#include "src/persistence/EEPROM_Manager.h"
#include "../../main/project_config.h"
#include <cstring>

namespace planetopia {
namespace mesh {

using namespace planetopia::utils;

bool Mesh::isReplay(const mesh_message& msg) {
  for (size_t i = 0; i < REPLAY_CACHE_SIZE; ++i) {
    if (memcmp(replayCache[i].mac, msg.originMacAddress, 6) == 0 &&
        replayCache[i].epoch == msg.epochNum && replayCache[i].seq == msg.seqNum) {
      return true;
    }
  }
  // Record this entry in the ring buffer
  memcpy(replayCache[replayCacheIdx].mac, msg.originMacAddress, 6);
  replayCache[replayCacheIdx].epoch = msg.epochNum;
  replayCache[replayCacheIdx].seq = msg.seqNum;
  replayCacheIdx = (replayCacheIdx + 1) % REPLAY_CACHE_SIZE;
  return false;
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
      Logger::logln("MESH", "Beacon from unexpected MAC rejected (master still alive)",
                    LogLevel::LOG_WARN);
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
    Logger::logln("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  }

  if (planetopia::utils::MacAddress(lastSeenMasterMac) !=
          planetopia::utils::MacAddress(msg.originMacAddress) &&
      lastSeenMasterMac[0] != 0) {
    Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
  }
  memcpy(lastSeenMasterMac, msg.originMacAddress, 6);
  lastMasterBeaconReceivedMs = millis();

  uint8_t newDistance = msg.hopCount + 1;
  if (currentMaster.distance == 0xFF ||
      planetopia::utils::MacAddress(currentMaster.mac) !=
          planetopia::utils::MacAddress(msg.originMacAddress) ||
      newDistance < currentMaster.distance) {
    memcpy(currentMaster.mac, msg.originMacAddress, 6);
    currentMaster.distance = newDistance;
    memcpy(currentMaster.nextHop, msg.lastHopMacAddress, 6);
  }

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    bool isNewer = (msg.epochNum > lastRelayedEpoch) ||
                   (msg.epochNum == lastRelayedEpoch && msg.seqNum > lastRelayedSeqNum);
    if (!isNewer) {
      Logger::logln("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    lastRelayedEpoch = msg.epochNum;
    lastRelayedSeqNum = msg.seqNum;

    // Defer relay with jitter (esp_random() returns deterministic 42 in tests)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % planetopia::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hopCount = newDistance;
    memcpy(relayPendingMsg.lastHopMacAddress, deviceMacAddress, 6);
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

    // Update last-seen for known peers only
    updatePeerLastSeen(entry.srcMac);

    switch (msg.messageType) {
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
  if (planetopia::utils::MacAddress(target) == planetopia::utils::MacAddress(deviceMacAddress)) {
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
  if (msg.hopCount >= planetopia::config::MAX_HOPS)
    return;
  mesh_message relay = msg;
  relay.hopCount++;
  memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue;
    sendMessage(peerMacs[i].mac, relay);
  }
}

void Mesh::processJoinAck(const mesh_message& msg) {
  // Relay outward if not addressed to us (multi-hop enrollment)
  if (memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0) {
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
    memcpy(knownMasterMac, msg.originMacAddress, 6);
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
    memcpy(msg.targetMacAddress, currentMaster.mac, 6);
  }

  // Routing: always use next hop if possible
  PeerInfo* nextHop = findNextHopToMaster();
  if (nextHop && planetopia::utils::MacAddress(nextHop->mac) !=
                     planetopia::utils::MacAddress(deviceMacAddress)) {
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
  return millis() - peer->lastSeenMillis < planetopia::config::STALE_PEER_THRESHOLD_MS;
}

PeerInfo* Mesh::findNextHopToMaster() {
  // For this mesh: nextHop == currentMaster.nextHop
  if (currentMaster.distance == 0xFF)
    return nullptr;
  for (size_t i = 0; i < peerCount; ++i) {
    if (planetopia::utils::MacAddress(peerMacs[i].mac) ==
            planetopia::utils::MacAddress(currentMaster.nextHop) &&
        isPeerInRange(peerMacs[i].mac) &&
        planetopia::utils::MacAddress(peerMacs[i].mac) !=
            planetopia::utils::MacAddress(deviceMacAddress))
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
  memset(msg.targetMacAddress, 0xFF, 6); // broadcast indicator — relayed by all intermediate nodes
  broadcastToAllPeers(msg);
}

void Mesh::linkDataRecvCallback(std::function<void(mesh_message)> recvCallback) {
  externalRecvCallback = recvCallback;
}

void Mesh::processAdapterData(const mesh_message& msg) {
  static constexpr uint8_t OP_CONFIG_SET = 0xA0;
  static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  bool addressedToSelf = (memcmp(msg.targetMacAddress, deviceMacAddress, 6) == 0);
  bool isBroadcastTarget = (memcmp(msg.targetMacAddress, kBroadcastMac, 6) == 0);
  bool addressedToMaster =
      hasMasterMac && (memcmp(msg.targetMacAddress, currentMaster.mac, 6) == 0);

  if (!isMaster && !addressedToSelf && !isBroadcastTarget) {
    if (addressedToMaster) {
      // Uplink: relay toward master via routing table
      if (msg.hopCount >= planetopia::config::MAX_HOPS)
        return;
      mesh_message relay = msg;
      relay.hopCount++;
      memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
      transmitCore(relay.dataType, relay.data, MESH_TYPE_ADAPTER_DATA, &relay);
      return;
    }
    // Downlink to another node: relay outward toward specific target
    relayDownlink(msg);
    return;
  }

  // Local delivery
  bool isConfigOpcode =
      (msg.dataType == adapter_types::SERIAL_ADAPTER && msg.data[0] == OP_CONFIG_SET);
  if (isConfigOpcode && hasMasterMac && memcmp(msg.originMacAddress, knownMasterMac, 6) != 0) {
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
  msg.messageType = MESH_TYPE_ENROLLMENT;
  msg.dataType = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.originMacAddress, deviceMacAddress, 6);
  memset(msg.targetMacAddress, 0xFF, 6);
  memcpy(msg.lastHopMacAddress, deviceMacAddress, 6);
  msg.hopCount = 0;
  memcpy(msg.enrollmentPublicKey, devicePublicKey, 32);

  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  Logger::logln("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}

void Mesh::processEnrollmentRequest(const mesh_message& msg) {
  if (!isMaster) {
    return;
  }
  memcpy(_pendingEnrollmentMac, msg.originMacAddress, 6);
  memcpy(_pendingEnrollmentPubKey, msg.enrollmentPublicKey, 32);
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
} // namespace planetopia
