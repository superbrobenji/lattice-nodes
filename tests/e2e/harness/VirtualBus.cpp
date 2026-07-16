#include "VirtualBus.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "NodeContext.h"
#include "esp_now_mock.h"

namespace sim {

void VirtualBus::addNode(SimNode* n) {
  nodes_.push_back(n);
}

void VirtualBus::link(SimNode* a, SimNode* b) {
  if (linked(a, b)) return;
  links_.emplace_back(a, b);
}

void VirtualBus::unlink(SimNode* a, SimNode* b) {
  links_.erase(std::remove_if(links_.begin(), links_.end(),
                               [&](const std::pair<SimNode*, SimNode*>& p) {
                                 return (p.first == a && p.second == b) ||
                                        (p.first == b && p.second == a);
                               }),
               links_.end());
}

bool VirtualBus::linked(SimNode* a, SimNode* b) const {
  for (const auto& p : links_) {
    if ((p.first == a && p.second == b) || (p.first == b && p.second == a)) return true;
  }
  return false;
}

size_t VirtualBus::framesInFlight() const {
  return pending_.size();
}

SimNode* VirtualBus::findByMac(const uint8_t* mac) const {
  for (SimNode* n : nodes_) {
    if (memcmp(n->mac(), mac, 6) == 0) return n;
  }
  return nullptr;
}

namespace {
// Firmware always calls esp_now_send(broadcastMac, ...) with an explicit
// FF:FF:FF:FF:FF:FF address rather than esp_now_send(nullptr, ...), so the
// mock's EspNowSend::isBroadcast (peer_addr == nullptr) is never true for
// real broadcasts. Recognize the sentinel address too.
bool isBroadcastAddr(const uint8_t* addr) {
  static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return memcmp(addr, kBroadcast, 6) == 0;
}
} // namespace

void VirtualBus::deliver() {
  // Phase 1: deliver frames captured on the previous step
  auto delivering = std::move(pending_);
  pending_.clear();
  for (auto& f : delivering) {
    NodeContext& ctx = f.target->ctx();
    if (!ctx.espNowRecvCb) continue;
    swapIn(ctx);
    simulateReceive(f.src, f.data.data(), static_cast<int>(f.data.size()));
    swapOut(ctx);
  }
  // Phase 2: collect this step's sends into pending
  for (SimNode* sender : nodes_) {
    auto enqueue = [&](SimNode* target, const std::vector<uint8_t>& data) {
      Pending p{};
      p.target = target;
      memcpy(p.src, sender->mac(), 6);
      p.data = data;
      pending_.push_back(std::move(p));
    };
    auto& sent = sender->ctx().espNowSent;
    for (auto& pkt : sent) {
      if (pkt.isBroadcast || isBroadcastAddr(pkt.addr)) {
        for (SimNode* other : nodes_)
          if (other != sender && linked(sender, other)) enqueue(other, pkt.data);
      } else {
        SimNode* target = findByMac(pkt.addr);
        // Frame to a MAC no node owns = routing corruption — fail loudly
        if (!target) throw std::runtime_error("VirtualBus: frame to unknown MAC");
        if (linked(sender, target)) enqueue(target, pkt.data);
        // unlinked unicast: silently lost, like RF out of range
      }
    }
    sent.clear();
  }
}

} // namespace sim
