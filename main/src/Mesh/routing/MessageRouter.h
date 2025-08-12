#ifndef MESSAGE_ROUTER_H
#define MESSAGE_ROUTER_H

#include <functional>
#include <vector>
#include "src/Mesh/Mesh.h"

namespace planetopia {
namespace utils {

class MessageRouter {
public:
  // Message routing strategies
  enum class RoutingStrategy {
    TO_MASTER,               // Route message to master node
    TO_TARGET,               // Route message to specific target
    BROADCAST,               // Broadcast to all peers
    BROADCAST_EXCEPT_ORIGIN  // Broadcast to all except origin
  };

  // Message routing result
  struct RoutingResult {
    bool success;
    std::vector<uint8_t*> nextHops;  // MAC addresses of next hops
    uint8_t distance;                // Distance to target
    String errorMessage;
  };

  // Constructor
  MessageRouter();

  // Core routing methods
  RoutingResult routeMessage(const planetopia::mesh::mesh_message& message,
                             const std::vector<planetopia::mesh::PeerInfo>& peers,
                             const planetopia::mesh::MasterInfo& masterInfo,
                             const uint8_t* ownMac);

  // Helper methods
  bool shouldRouteToMaster(const planetopia::mesh::mesh_message& message) const;
  bool shouldBroadcast(const planetopia::mesh::mesh_message& message) const;
  std::vector<uint8_t*> findNextHopsToTarget(const uint8_t* targetMac,
                                             const std::vector<planetopia::mesh::PeerInfo>& peers,
                                             const planetopia::mesh::MasterInfo& masterInfo) const;

  // Configuration
  void setMaxHops(uint8_t maxHops) {
    maxHops_ = maxHops;
  }
  void setRoutingTimeout(unsigned long timeout) {
    routingTimeout_ = timeout;
  }

private:
  uint8_t maxHops_;
  unsigned long routingTimeout_;

  // Internal routing logic
  bool isValidTarget(const uint8_t* targetMac) const;
  bool isOwnMac(const uint8_t* mac1, const uint8_t* mac2) const;
  uint8_t calculateDistance(const uint8_t* fromMac, const uint8_t* toMac,
                            const std::vector<planetopia::mesh::PeerInfo>& peers) const;
};

}  // namespace utils
}  // namespace planetopia

#endif  // MESSAGE_ROUTER_H
