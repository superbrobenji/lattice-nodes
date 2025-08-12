#include "MessageRouter.h"
#include "src/core/Logger.h"
#include <cstring>
#include "src/Mesh/Mesh.h"
using planetopia::mesh::PeerInfo;
using planetopia::mesh::MasterInfo;

namespace planetopia {
namespace utils {

MessageRouter::MessageRouter()
  : maxHops_(10), routingTimeout_(5000) {
}

MessageRouter::RoutingResult MessageRouter::routeMessage(
  const planetopia::mesh::mesh_message& message,
  const std::vector<PeerInfo>& peers,
  const MasterInfo& masterInfo,
  const uint8_t* ownMac) {

  RoutingResult result = {};

  // Check if message has exceeded max hops
  if (message.hopCount >= maxHops_) {
    result.success = false;
    result.errorMessage = "Message exceeded max hops";
    Logger::logln("ROUTER", "Message exceeded max hops: " + String(message.hopCount), LogLevel::LOG_WARN);
    return result;
  }

  // Determine routing strategy based on message type and content
  if (shouldBroadcast(message)) {
    // Broadcast to all peers except origin
    for (const auto& peer : peers) {
      if (!isOwnMac(peer.mac, ownMac) && !isOwnMac(peer.mac, message.originMacAddress)) {
        result.nextHops.push_back(const_cast<uint8_t*>(peer.mac));
      }
    }
    result.success = true;
    result.distance = 1;
    Logger::logln("ROUTER", "Broadcasting message to " + String(result.nextHops.size()) + " peers", LogLevel::LOG_DEBUG);

  } else if (shouldRouteToMaster(message)) {
    // Route to master node
    if (masterInfo.distance != 0xFF && !isOwnMac(masterInfo.mac, ownMac)) {
      result.nextHops.push_back(const_cast<uint8_t*>(masterInfo.nextHop));
      result.success = true;
      result.distance = masterInfo.distance;
      Logger::logln("ROUTER", "Routing message to master via next hop", LogLevel::LOG_DEBUG);
    } else {
      result.success = false;
      result.errorMessage = "No route to master available";
      Logger::logln("ROUTER", "No route to master available", LogLevel::LOG_WARN);
    }

  } else {
    // Route to specific target
    if (isValidTarget(message.targetMacAddress)) {
      auto nextHops = findNextHopsToTarget(message.targetMacAddress, peers, masterInfo);
      if (!nextHops.empty()) {
        result.nextHops = nextHops;
        result.success = true;
        result.distance = calculateDistance(ownMac, message.targetMacAddress, peers);
        Logger::logln("ROUTER", "Routing message to target via " + String(nextHops.size()) + " next hops", LogLevel::LOG_DEBUG);
      } else {
        result.success = false;
        result.errorMessage = "No route to target available";
        Logger::logln("ROUTER", "No route to target available", LogLevel::LOG_WARN);
      }
    } else {
      result.success = false;
      result.errorMessage = "Invalid target MAC address";
      Logger::logln("ROUTER", "Invalid target MAC address", LogLevel::LOG_WARN);
    }
  }

  return result;
}

bool MessageRouter::shouldRouteToMaster(const planetopia::mesh::mesh_message& message) const {
  // Route to master if:
  // 1. Message type is adapter data (not beacon)
  // 2. Target is broadcast (0xFF...)
  // 3. Not a broadcast message type
  return (message.messageType == planetopia::mesh::MESH_TYPE_ADAPTER_DATA && message.dataType != planetopia::adapter::SERIAL_ADAPTER);
}

bool MessageRouter::shouldBroadcast(const planetopia::mesh::mesh_message& message) const {
  // Broadcast if:
  // 1. Message type is master beacon
  // 2. Serial adapter broadcast message
  // 3. Target is broadcast (0xFF...)
  return (message.messageType == planetopia::mesh::MESH_TYPE_MASTER_BEACON || message.dataType == planetopia::adapter::SERIAL_ADAPTER || (message.targetMacAddress[0] == 0xFF && message.targetMacAddress[1] == 0xFF));
}

std::vector<uint8_t*> MessageRouter::findNextHopsToTarget(
  const uint8_t* targetMac,
  const std::vector<PeerInfo>& peers,
  const MasterInfo& masterInfo) const {

  std::vector<uint8_t*> nextHops;

  // If target is master, use master routing info
  if (memcmp(targetMac, masterInfo.mac, 6) == 0) {
    if (masterInfo.distance != 0xFF) {
      nextHops.push_back(const_cast<uint8_t*>(masterInfo.nextHop));
    }
    return nextHops;
  }

  // Check if target is a direct peer
  for (const auto& peer : peers) {
    if (memcmp(peer.mac, targetMac, 6) == 0) {
      nextHops.push_back(const_cast<uint8_t*>(peer.mac));
      return nextHops;
    }
  }

  // For now, if target is not a direct peer, route through master
  // This could be enhanced with more sophisticated routing algorithms
  if (masterInfo.distance != 0xFF) {
    nextHops.push_back(const_cast<uint8_t*>(masterInfo.nextHop));
  }

  return nextHops;
}

bool MessageRouter::isValidTarget(const uint8_t* targetMac) const {
  // Check if MAC is not all zeros or all 0xFF
  bool allZero = true;
  bool allFF = true;

  for (int i = 0; i < 6; ++i) {
    if (targetMac[i] != 0x00) allZero = false;
    if (targetMac[i] != 0xFF) allFF = false;
  }

  return !allZero && !allFF;
}

bool MessageRouter::isOwnMac(const uint8_t* mac1, const uint8_t* mac2) const {
  return memcmp(mac1, mac2, 6) == 0;
}

uint8_t MessageRouter::calculateDistance(const uint8_t* fromMac, const uint8_t* toMac,
                                         const std::vector<PeerInfo>& peers) const {
  // Simple distance calculation - could be enhanced with actual network topology
  if (memcmp(fromMac, toMac, 6) == 0) {
    return 0;  // Same node
  }

  // Check if direct peer
  for (const auto& peer : peers) {
    if (memcmp(peer.mac, toMac, 6) == 0) {
      return 1;  // Direct peer
    }
  }

  return 2;  // Default: 2 hops (through master)
}

}  // namespace utils
}  // namespace planetopia
