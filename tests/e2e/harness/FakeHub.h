#pragma once
// FakeHub is a scripted stand-in for the real hub/server process that normally
// sits on the far end of the master node's physical serial cable. It reads
// framed nanopb mesh_message bytes off the master's SimNode::ctx().serialWritten
// mock buffer (decoding via its own nanopb decode in FakeHub.cpp -- see that
// file for why it can't reuse SerialFraming::decode(), which is written from a
// device's perspective and overwrites origin MACs) and can inject scripted
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

  // The hub's own identity key, stamped into the serial JOIN_ACK's public_key
  // field exactly as the real orchestrator does (server.go ApproveEnrollment:
  // PublicKey = ms.masterPublicKey, loaded from its data/masterkey.json).
  // Deliberately NOT any SimNode's on-device key and NOT the test pin: the real
  // hub's key is self-generated on the hub, no board holds its private half,
  // and the master firmware must ignore it when building the mesh-side
  // JOIN_ACK (MeshMessenger::enrollPeer stamps its own enrollment.getPublicKey()
  // instead — #126). Modelling the real wire contract here is what lets the
  // e2e suite catch a regression to echoing it: the leaf's pin check would
  // drop the ACK, and even with the pin bypassed E2E would never agree.
  static constexpr uint8_t HUB_IDENTITY_PUBLIC_KEY[32] = {
      0x7C, 0x0E, 0x93, 0x51, 0xD8, 0x2B, 0x6F, 0xA4, 0x15, 0xC7, 0x38,
      0x9A, 0xE2, 0x64, 0xB0, 0x0D, 0x5F, 0x81, 0x2C, 0xF6, 0x49, 0xAB,
      0x73, 0x1E, 0x96, 0x08, 0xD4, 0x3B, 0x67, 0xCA, 0x52, 0x1F,
  };

  // Script a server-approved enrollment, mirroring the real hub's JOIN_ACK
  // (server.go ApproveEnrollment): addressed to nodeMac, origin = the master's
  // MAC, enrollment_public_key = HUB_IDENTITY_PUBLIC_KEY (the hub's own key,
  // NOT the node's), and the first 4 bytes of nodePubKey32 in data[0..3]. That
  // fingerprint is NOT checked by the master's serial handler
  // (SerialAdapter::handleCompleteFrame only checks enrollment_public_key
  // non-zero) -- it's consumed later by the enrolling NODE, when
  // Mesh::enrollPeer relays its own JOIN_ACK over the mesh and
  // Enrollment::processJoinAck there checks the fingerprint against that
  // node's own device public key. The master registers the node under the key
  // it cached from the node's JOIN_REQUEST, never under anything in this frame.
  void approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32);

  // Same as above, but also tells the node about a server-designated secondary
  // master (Phase 4 dual-master failover): sets ack.data[4..42] (protocol
  // v0.6.0 wire shrink §8 — secondary MAC + pubkey, previously top-level
  // fields) before injecting the JOIN_ACK. SerialAdapter::
  // handleCompleteFrame forwards these into the 4-arg Mesh::enrollPeer overload,
  // which relays them on to the enrolling node's own JOIN_ACK, where
  // Enrollment::processJoinAck registers the secondary as a peer (so
  // masterE2EKeys can derive a key against it after failover) and records it
  // for beacon adoption. The 2-arg overload above delegates here with an
  // all-zero secondary, which SerialAdapter treats as "no secondary present".
  void approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32,
                         const uint8_t* secondaryMac, const uint8_t* secondaryPubKey32);

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
