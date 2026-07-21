#include "FakeHub.h"
#include <cstring>
#include <stdexcept>
#include "src/adapter/serial/SerialFraming.h"
#include "src/adapter/AdapterFactory.h"
#include "src/mesh/serialization/mesh.pb.h"
#include "src/mesh/serialization/nanopb/pb_decode.h"
#include "lib/lattice-protocol/c/opcodes.h"

namespace sim {

namespace {
const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// FakeHub stands in for the server on the far end of the master's serial
// cable, and (unlike a real device) has no mesh identity of its own.
// SerialFraming::decode() is written from a DEVICE's perspective: for any
// message type other than JOIN_ACK/SERIAL_CMD_BROADCAST it discards the
// wire-encoded origin/last-hop MACs and substitutes esp_wifi_get_mac()'s
// CURRENT value, on the assumption "this is my own mac, so any frame with a
// blank origin must be mine". That assumption doesn't hold for a server doing
// its own decode outside of any device context -- esp_wifi_get_mac() there
// just returns whichever SimNode context the harness last had swapped in
// globally, not the frame's actual originator. FakeHub's enrollmentFrom()/
// adapterDataFromOrigin() need the origin exactly as it appeared on the wire,
// so decode it here directly (mirroring SerialFraming::decode()'s field
// copying field-for-field) instead of going through that device-side helper.
bool decodeWireFrame(const uint8_t* data, size_t len, mesh_message& outMsg) {
  memset(&outMsg, 0, sizeof(outMsg));

  mesh_MeshMessage pbMsg = mesh_MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, mesh_MeshMessage_fields, &pbMsg))
    return false;

  outMsg.message_type = static_cast<::MeshMessageType>(pbMsg.messageType);
  outMsg.data_type = static_cast<lattice::adapter::adapter_types>(pbMsg.dataType);
  outMsg.hop_count = static_cast<uint8_t>(pbMsg.hopCount);
  outMsg.epoch_num = pbMsg.epochNum;
  outMsg.seq_num = static_cast<uint16_t>(pbMsg.seqNum);
  outMsg.proto_version = static_cast<uint8_t>(pbMsg.protoVersion);
  memcpy(outMsg.target_mac_address, pbMsg.targetMacAddress, 6);
  // Unlike SerialFraming::decode(), always take origin/last-hop from the wire —
  // this is the whole reason FakeHub has its own decode path (see above).
  memcpy(outMsg.origin_mac_address, pbMsg.originMacAddress, 6);
  memcpy(outMsg.last_hop_mac_address, pbMsg.lastHopMacAddress, 6);

  if (pbMsg.has_data) {
    size_t dataToCopy =
        pbMsg.data.size < sizeof(outMsg.data) ? pbMsg.data.size : sizeof(outMsg.data);
    memcpy(outMsg.data, pbMsg.data.bytes, dataToCopy);
  }

  if (pbMsg.has_public_key) {
    memcpy(outMsg.enrollment_public_key, pbMsg.public_key.bytes, 32);
  }

  return true;
}
} // namespace

FakeHub::FakeHub(SimNode* master) : master_(master) {}

void FakeHub::poll() {
  auto& written = master_->ctx().serialWritten;
  rxBuffer_.insert(rxBuffer_.end(), written.begin(), written.end());
  written.clear();

  size_t off = 0;
  while (rxBuffer_.size() - off >= 2) {
    uint16_t len = static_cast<uint16_t>(rxBuffer_[off] | (rxBuffer_[off + 1] << 8));
    if (rxBuffer_.size() - off - 2 < len)
      break; // partial frame — wait for more bytes
    mesh_message msg{};
    if (decodeWireFrame(rxBuffer_.data() + off + 2, len, msg))
      received.push_back(msg);
    off += 2 + len;
  }
  rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<long>(off));
}

void FakeHub::sendFrame(const mesh_message& msg) {
  uint8_t encoded[256];
  size_t n = lattice::adapter::serial::SerialFraming::encode(msg, encoded, sizeof(encoded));
  if (n == 0)
    throw std::runtime_error("FakeHub: encode failed");
  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  auto& rx = master_->ctx().serialRx;
  rx.insert(rx.end(), lenLE, lenLE + 2);
  rx.insert(rx.end(), encoded, encoded + n);
}

void FakeHub::approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32) {
  static const uint8_t zeroMac[6] = {0, 0, 0, 0, 0, 0};
  static const uint8_t zeroPub[32] = {0};
  approveEnrollment(nodeMac, nodePubKey32, zeroMac, zeroPub);
}

void FakeHub::approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32,
                                const uint8_t* secondaryMac, const uint8_t* secondaryPubKey32) {
  mesh_message ack{};
  ack.proto_version = 2;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.target_mac_address, nodeMac, 6);
  memcpy(ack.enrollment_public_key, nodePubKey32, 32);
  // data[0..3] fingerprint: NOT checked here. SerialAdapter::handleCompleteFrame
  // (the master's serial-side JOIN_ACK handler this frame drives) only checks
  // enrollment_public_key non-zero before calling Mesh::enrollPeer. That call
  // in turn sends its OWN mesh-side JOIN_ACK (over ESP-NOW, master -> enrolling
  // node) with this same pubkey's first 4 bytes as the fingerprint; it is THAT
  // later frame's data[0..3] that Enrollment::processJoinAck checks, on the
  // enrolling node, against its own device public key.
  memcpy(ack.data, nodePubKey32, 4);
  // Server-designated secondary master (Phase 4 dual-master failover). Left
  // zeroed by the 2-arg overload above, which SerialAdapter::handleCompleteFrame
  // treats as "no secondary present" (see its all-zero secondary_master_mac
  // check) and so falls back to the plain 2-arg Mesh::enrollPeer.
  memcpy(ack.secondary_master_mac, secondaryMac, 6);
  memcpy(ack.secondary_public_key, secondaryPubKey32, 32);
  sendFrame(ack);
}

void FakeHub::sendConfigSet(const uint8_t* targetMac, lattice::adapter::adapter_types newType) {
  mesh_message msg{};
  msg.proto_version = 2;
  msg.message_type = MESH_TYPE_SERIAL_CMD_BROADCAST;
  msg.data_type = lattice::adapter::SERIAL_ADAPTER;
  memcpy(msg.target_mac_address, targetMac, 6);
  msg.data[0] = OP_CONFIG_SET;
  memcpy(&msg.data[1], targetMac, 6);
  msg.data[7] = lattice::adapter::AdapterFactory::adapterTypeToEEPROM(newType);
  sendFrame(msg);
}

void FakeHub::sendHealthReq() {
  mesh_message msg{};
  msg.proto_version = 2;
  msg.message_type = MESH_TYPE_SERIAL_CMD_BROADCAST;
  msg.data_type = lattice::adapter::SERIAL_ADAPTER;
  memcpy(msg.target_mac_address, kBroadcastMac, 6);
  msg.data[0] = OP_HEALTH_REQ;
  sendFrame(msg);
}

std::vector<mesh_message> FakeHub::ofType(MeshMessageType t) const {
  std::vector<mesh_message> out;
  for (const auto& m : received)
    if (m.message_type == t)
      out.push_back(m);
  return out;
}

const mesh_message* FakeHub::enrollmentFrom(const uint8_t* mac) const {
  for (const auto& m : received) {
    if (m.message_type == MESH_TYPE_ENROLLMENT && memcmp(m.origin_mac_address, mac, 6) == 0)
      return &m;
  }
  return nullptr;
}

std::vector<mesh_message> FakeHub::adapterDataFromOrigin(const uint8_t* mac) const {
  std::vector<mesh_message> out;
  for (const auto& m : received) {
    if (m.message_type == MESH_TYPE_ADAPTER_DATA && memcmp(m.origin_mac_address, mac, 6) == 0)
      out.push_back(m);
  }
  return out;
}

} // namespace sim
