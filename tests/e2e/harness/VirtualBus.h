#pragma once
// VirtualBus models RF-range topology between SimNodes: a frame sent by one
// node is only delivered to another node if the two are `link()`ed. Delivery
// is deferred by one step (frames sent during step N land in step N+1's
// deliver() call) so nodes never observe another node's send within the same
// tick — mirroring real async ESP-NOW timing.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include "SimNode.h"
#include "lib/lattice-protocol/c/mesh_message.h"
#include "lib/lattice-protocol/c/message_types.h"

namespace sim {

class VirtualBus {
public:
  void addNode(SimNode* n);
  void link(SimNode* a, SimNode* b);   // bidirectional reachability
  void unlink(SimNode* a, SimNode* b);
  bool linked(SimNode* a, SimNode* b) const;

  // Drain every node's captured sends into pending; deliver pending frames
  // (from the PREVIOUS step) into target nodes' recv callbacks.
  void deliver();
  size_t framesInFlight() const;

  // Test-only accessor (harness code, not firmware): a frame sent during step
  // N sits in `pending_` from the moment step N's deliver() call returns until
  // step N+1's deliver() call delivers it. This lets an e2e test inspect or
  // tamper an in-flight, already-sealed frame in that one-step window (e.g.
  // Task 6's E2E AEAD tamper/forgery scenarios). Returns a pointer to the raw
  // bytes of the most recently enqueued pending frame of the given type, or
  // nullptr if none is currently pending.
  mesh_message* lastPendingOfType(MeshMessageType type);

private:
  struct Pending {
    SimNode* target;
    uint8_t src[6];
    std::vector<uint8_t> data;
  };

  std::vector<SimNode*> nodes_;
  std::vector<std::pair<SimNode*, SimNode*>> links_;
  std::vector<Pending> pending_;

  SimNode* findByMac(const uint8_t* mac) const;
};

} // namespace sim
