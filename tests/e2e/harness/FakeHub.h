#pragma once
// FakeHub is a scripted stand-in for the real hub/server process that normally
// sits on the far end of the master node's physical serial cable. It reads
// framed nanopb mesh_message bytes off the master's SimNode::ctx().serialWritten
// mock buffer (decoding via SerialFraming::decode) and can inject scripted
// frames back onto the master's ctx().serialRx mock buffer (via
// SerialFraming::encode), exactly as a real hub process would over UART.
#include <cstdint>
#include <vector>
#include "SimNode.h"
#include "src/adapter/Adapter.h"
#include "lib/lattice-protocol/c/mesh_message.h"
#include "lib/lattice-protocol/c/message_types.h"

namespace sim {

class FakeHub {
public:
  explicit FakeHub(SimNode* master);

  // Drain master_->ctx().serialWritten into rxBuffer_, split complete
  // [len][nanopb] frames off, decode each, and append to received. Partial
  // frames are retained across poll() calls.
  void poll();

  std::vector<mesh_message> received;

  // Encode msg and inject it (length-prefixed) into the master's serialRx
  // mock buffer, exactly as bytes arriving over a real UART would.
  void sendFrame(const mesh_message& msg);

  // Script a server-approved enrollment: JOIN_ACK addressed to nodeMac, with
  // the node's own pubkey echoed back in enrollment_public_key (so
  // Mesh::enrollPeer registers it) and its first 4 bytes duplicated into
  // data[0..3] as the fingerprint Enrollment::processJoinAck checks.
  void approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32);

  // Script a server-issued adapter reconfiguration for targetMac.
  void sendConfigSet(const uint8_t* targetMac, lattice::adapter::adapter_types newType);

  // Script a broadcast health request (target = FF:FF:FF:FF:FF:FF).
  void sendHealthReq();

  // Query helpers over `received`.
  std::vector<mesh_message> ofType(MeshMessageType t) const;
  const mesh_message* enrollmentFrom(const uint8_t* mac) const; // nullptr if none
  std::vector<mesh_message> adapterDataFromOrigin(const uint8_t* mac) const;

private:
  SimNode* master_;
  std::vector<uint8_t> rxBuffer_; // accumulates partial frames across poll()s
};

} // namespace sim
