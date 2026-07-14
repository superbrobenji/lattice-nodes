#pragma once
// SimWorld owns a set of SimNodes plus the VirtualBus connecting them, and
// drives them together: each step() advances the shared clock, ticks every
// node (rebooting any that requested a restart, e.g. the OP_CONFIG_SET
// hotswap path), then delivers frames sent during that tick.
#include <cstdint>
#include <memory>
#include <vector>
#include "SimClock.h"
#include "SimNode.h"
#include "VirtualBus.h"

namespace sim {

class SimWorld {
public:
  SimClock clock;
  VirtualBus bus;

  SimNode* addNode(const NodeConfig& cfg); // owns, boots, registers on bus
  void step(uint32_t ms = 1); // advance clock, tick all nodes, bus.deliver(), handle restarts
  void run(uint32_t ms);      // step() in a loop

private:
  std::vector<std::unique_ptr<SimNode>> nodes_;
};

} // namespace sim
