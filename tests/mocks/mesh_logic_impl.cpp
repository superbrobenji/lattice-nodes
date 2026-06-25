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
  }
  memcpy(lastSeenMasterMac, msg.originMacAddress, 6);
  lastMasterBeaconReceivedMs = millis();

  uint8_t newDistance = msg.hopCount + 1;
  if (currentMaster.distance == 0xFF || planetopia::utils::MacAddress(currentMaster.mac) != planetopia::utils::MacAddress(msg.originMacAddress) || newDistance < currentMaster.distance) {
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
    lastRelayedEpoch  = msg.epochNum;
    lastRelayedSeqNum = msg.seqNum;

    // Defer relay with jitter (esp_random() returns deterministic 42 in tests)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % planetopia::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hopCount = newDistance;
    memcpy(relayPendingMsg.lastHopMacAddress, deviceMacAddress, 6);
    relayPendingAt = millis() + 10 + jitterMs;
    relayPending   = true;
  }
}

}  // namespace mesh
}  // namespace planetopia
