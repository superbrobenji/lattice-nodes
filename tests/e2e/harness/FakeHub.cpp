#include "FakeHub.h"
#include <cstring>
#include <stdexcept>
#include "src/adapter/serial/SerialFraming.h"
#include "src/adapter/AdapterFactory.h"
#include "lib/lattice-protocol/c/opcodes.h"

namespace sim {

namespace {
const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

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
    if (lattice::adapter::serial::SerialFraming::decode(rxBuffer_.data() + off + 2, len, msg))
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
  mesh_message ack{};
  ack.proto_version = 2;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.target_mac_address, nodeMac, 6);
  memcpy(ack.enrollment_public_key, nodePubKey32, 32);
  memcpy(ack.data, nodePubKey32, 4); // fingerprint checked by Enrollment::processJoinAck
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
