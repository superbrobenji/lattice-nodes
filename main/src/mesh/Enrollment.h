#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

using EnrollmentRelayFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);
using SendMessageFn = std::function<void(const uint8_t* target, const mesh_message&)>;
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

  void sendRequest(const uint8_t* deviceMac, SendMessageFn sendFn);
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
  volatile bool _pendingEnrollmentRelay{false};
  uint8_t _pendingEnrollmentMac[6]{};
  uint8_t _pendingEnrollmentPubKey[32]{};
  EnrollmentRelayFn _enrollmentRelayFn{nullptr};
};

} // namespace mesh
} // namespace lattice
