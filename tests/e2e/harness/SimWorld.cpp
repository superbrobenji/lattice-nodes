#include "SimWorld.h"

namespace sim {

SimNode* SimWorld::addNode(const NodeConfig& cfg) {
  nodes_.push_back(std::make_unique<SimNode>(cfg));
  SimNode* n = nodes_.back().get();
  n->boot();
  bus.addNode(n);
  return n;
}

void SimWorld::step(uint32_t ms) {
  clock.advance(ms);
  for (auto& n : nodes_) {
    n->tick();
    if (n->restartRequested()) n->reboot(); // OP_CONFIG_SET hotswap path
  }
  bus.deliver();
}

void SimWorld::run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; ++i) step(1);
}

} // namespace sim
