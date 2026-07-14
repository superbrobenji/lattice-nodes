#pragma once
// SimNode boots and ticks a real firmware node on host, inside its own swapped
// NodeContext (see NodeContext.h). It mirrors main/main.ino's setup()/loop()
// closely enough to exercise the real Mesh/AdapterFactory/EepromManager stack,
// with these deliberate omissions (e2e scope, not full main.ino parity):
//   - no SevenSegDisplay/DisplayManager
//   - no Buttons/ButtonHandler (both hold function-local statics that can't be
//     swapped per-node; buttons aren't e2e scope)
//   - no WDT config (the test harness has its own execution budget)
//   - no CPU frequency call
//   - no dev-mode branches (nodes always run the "production" path against a
//     seeded EEPROM image, matching a provisioned device)
//   - no pubkey serial print
#include <cstdint>
#include <memory>
#include "NodeContext.h"
#include "src/adapter/Adapter.h"
#include "src/adapter/AdapterFactory.h"

namespace lattice {
namespace mesh {
class Mesh;
}
namespace hardware {
class Led;
}
} // namespace lattice

namespace sim {

struct NodeConfig {
  uint8_t mac[6];
  bool isMaster;
  lattice::adapter::adapter_types adapterType;
};

class SimNode {
public:
  explicit SimNode(const NodeConfig& cfg);
  ~SimNode();

  void boot();     // setup()-equivalent inside own context; seeds EEPROM on first boot
  void tick();     // one loop() iteration inside own context; throws err::FatalError on fatal
  void reboot();   // keeps EEPROM image, reconstructs everything else

  bool restartRequested() const { return ctx_.espRestartRequested; }
  NodeContext& ctx() { return ctx_; }
  const uint8_t* mac() const { return cfg_.mac; }

  // Run fn with this node's globals swapped in (for assertions on mesh state)
  template <typename F> auto with(F fn) {
    swapIn(ctx_);
    auto result = fn(*mesh_, adapter_.get());
    swapOut(ctx_);
    return result;
  }

  bool isEnrolled();          // convenience: with() wrapper
  void simulatePirMotion();   // requires PIR adapter; with() wrapper around PirAdapter::simulateMotion()

private:
  NodeConfig cfg_;
  NodeContext ctx_;
  std::unique_ptr<lattice::mesh::Mesh> mesh_;
  std::unique_ptr<lattice::adapter::Adapter> adapter_;
  std::unique_ptr<lattice::hardware::Led> greenLed_, redLed_;
  uint32_t lastEnrollmentBroadcastMs_ = 0; // mirrors main.ino function-local static
  bool booted_ = false;
};

} // namespace sim
