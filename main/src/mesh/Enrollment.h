#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

using EnrollmentRelayFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);
// Returns false if the peer could not be registered (e.g. registry full).
using RegisterPeerFn = std::function<bool(const uint8_t* mac, const uint8_t* pubKey32)>;

class Enrollment {
public:
  // TOFU state (read by Mesh for beacon/config processing)
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};

  Enrollment();
  void init(); // loads or generates keypair; loads enrolled flag + TOFU MACs from EEPROM

  bool isEnrolled() const;
  const uint8_t* getPublicKey() const { return devicePublicKey; }
  const uint8_t* getPrivateKey() const { return devicePrivateKey; }

  void sendRequest(const uint8_t* deviceMac, uint8_t protoVersion, uint32_t epochNum,
                   uint16_t seqNum);
  void processRequest(const mesh_message& msg);
  void processJoinAck(const mesh_message& msg, const uint8_t* deviceMac, RegisterPeerFn registerFn);
  void enrollPeer(const uint8_t* mac, const uint8_t* pubKey32, RegisterPeerFn registerFn,
                  bool dualMasterMode);

  void setRelayFn(EnrollmentRelayFn fn);
  void setPendingRelay(const uint8_t* mac, const uint8_t* pubKey);
  void drainPendingRelay();

#ifdef UNIT_TEST
public:
#else
private:
#endif
  uint8_t devicePrivateKey[32]{};
  uint8_t devicePublicKey[32]{};

  // Bounded, heap-free FIFO of enrollment requests awaiting relay to the server.
  // Sized to RECV_QUEUE_SIZE (8): the master drains this queue once per loop()
  // AFTER draining its ESP-NOW receive ring, so the enrollment requests from one
  // drain pass accumulate here before being relayed in a batch. A single-slot latch
  // (the previous design) silently dropped all but the last when two nodes
  // enrolled concurrently — see Task 9b Bug #6.
  static constexpr size_t PENDING_RELAY_QUEUE_SIZE = 8;
  struct PendingRelay {
    uint8_t mac[6];
    uint8_t pubKey[32];
  };
  PendingRelay _pendingRelayQueue[PENDING_RELAY_QUEUE_SIZE]{};
  size_t _pendingRelayHead{0};  // index of oldest queued entry
  size_t _pendingRelayCount{0}; // number of queued entries

  EnrollmentRelayFn _enrollmentRelayFn{nullptr};

  // Append one pending relay; drops (with a LOG_WARN, never err::fail) if full.
  void enqueuePendingRelay(const uint8_t* mac, const uint8_t* pubKey);
};

} // namespace mesh
} // namespace lattice
