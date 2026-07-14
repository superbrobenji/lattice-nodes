#include "SerialFraming.h"
#include "src/mesh/serialization/mesh.pb.h"
#include "src/mesh/serialization/nanopb/pb_encode.h"
#include "src/mesh/serialization/nanopb/pb_decode.h"
#include "src/adapter/Adapter.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <esp_wifi.h>
#include <cstring>

namespace lattice {
namespace adapter {
namespace serial {

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

size_t SerialFraming::encode(const lattice::mesh::mesh_message& msg, uint8_t* out, size_t maxLen) {
  using namespace lattice::utils;
  Logger::logln("Serial_Adapter",
                "Encoding mesh message - Type: " + String((uint8_t)msg.message_type) +
                    " DataType: " + String(static_cast<int32_t>(msg.data_type)) +
                    " HopCount: " + String(msg.hop_count),
                LogLevel::LOG_DEBUG);

  mesh_MeshMessage pbMsg = mesh_MeshMessage_init_zero;
  pbMsg.messageType = static_cast<uint32_t>(msg.message_type);
  pbMsg.dataType = static_cast<int32_t>(msg.data_type);
  pbMsg.hopCount = msg.hop_count;
  pbMsg.epochNum = msg.epoch_num;
  pbMsg.seqNum = static_cast<uint32_t>(msg.seq_num);
  pbMsg.protoVersion = static_cast<uint32_t>(msg.proto_version);
  memcpy(pbMsg.originMacAddress, msg.origin_mac_address, 6);
  memcpy(pbMsg.targetMacAddress, msg.target_mac_address, 6);
  memcpy(pbMsg.lastHopMacAddress, msg.last_hop_mac_address, 6);

  // data field: always present (12 bytes)
  pbMsg.has_data = true;
  pbMsg.data.size = sizeof(msg.data);
  memcpy(pbMsg.data.bytes, msg.data, sizeof(msg.data));

  // public_key: only encode for enrollment-related message types when non-zero
  if (msg.message_type == MESH_TYPE_ENROLLMENT || msg.message_type == MESH_TYPE_JOIN_ACK) {
    bool nonZero = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollment_public_key[i]) {
        nonZero = true;
        break;
      }
    }
    if (nonZero) {
      pbMsg.has_public_key = true;
      pbMsg.public_key.size = 32;
      memcpy(pbMsg.public_key.bytes, msg.enrollment_public_key, 32);
    }
  }

  pb_ostream_t stream = pb_ostream_from_buffer(out, maxLen);
  if (!pb_encode(&stream, mesh_MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter", "nanopb encode failed", LogLevel::LOG_ERROR);
    return 0;
  }

  Logger::logln("Serial_Adapter",
                "Successfully encoded mesh message to " + String(stream.bytes_written) + " bytes",
                LogLevel::LOG_DEBUG);
  return stream.bytes_written;
}

bool SerialFraming::decode(const uint8_t* data, size_t len, lattice::mesh::mesh_message& outMsg) {
  using namespace lattice::utils;
  Logger::logln("Serial_Adapter", "Decoding protobuf message of " + String(len) + " bytes",
                LogLevel::LOG_DEBUG);

  memset(&outMsg, 0, sizeof(outMsg));

  mesh_MeshMessage pbMsg = mesh_MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, mesh_MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter", "nanopb decode failed", LogLevel::LOG_ERROR);
    return false;
  }

  outMsg.message_type = static_cast<::MeshMessageType>(pbMsg.messageType);
  outMsg.data_type = static_cast<lattice::adapter::adapter_types>(pbMsg.dataType);
  outMsg.hop_count = static_cast<uint8_t>(pbMsg.hopCount);
  outMsg.epoch_num = pbMsg.epochNum;
  outMsg.seq_num = static_cast<uint16_t>(pbMsg.seqNum);
  outMsg.proto_version = static_cast<uint8_t>(pbMsg.protoVersion);
  memcpy(outMsg.target_mac_address, pbMsg.targetMacAddress, 6);

  if (pbMsg.has_data) {
    size_t dataToCopy = pbMsg.data.size < 12u ? pbMsg.data.size : 12u;
    memcpy(outMsg.data, pbMsg.data.bytes, dataToCopy);
  }

  if (pbMsg.has_public_key) {
    memcpy(outMsg.enrollment_public_key, pbMsg.public_key.bytes, 32);
  }

  // For server-to-device messages (JOIN_ACK, SERIAL_CMD_BROADCAST) the MAC
  // fields on the wire are meaningful and must not be overwritten.
  // For device-originated relays (ADAPTER_DATA, MASTER_BEACON) the server
  // leaves routing fields blank, so we fill them in with our own MAC.
  if (outMsg.message_type != MESH_TYPE_JOIN_ACK &&
      outMsg.message_type != MESH_TYPE_SERIAL_CMD_BROADCAST) {
    readOwnMac(outMsg.origin_mac_address);
    readOwnMac(outMsg.last_hop_mac_address);
  } else {
    memcpy(outMsg.origin_mac_address, pbMsg.originMacAddress, 6);
    memcpy(outMsg.last_hop_mac_address, pbMsg.lastHopMacAddress, 6);
  }

  Logger::logln("Serial_Adapter", "Successfully decoded protobuf message", LogLevel::LOG_DEBUG);
  return true;
}

bool SerialFraming::injectByte(uint8_t byteIn) {
  using namespace lattice::utils;
  switch (frameState) {
  case FrameState::AwaitingLen1:
    frameIndex = 0; // reset from any previously completed frame
    frameLength = byteIn;
    frameState = FrameState::AwaitingLen2;
    break;

  case FrameState::AwaitingLen2:
    frameLength |= static_cast<uint16_t>(byteIn) << 8;
    if (frameLength == 0 || frameLength > MAX_PAYLOAD) {
      Logger::logln("SERIAL", "Frame parse error", LogLevel::LOG_WARN);
      lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER,
                         2, "Serial_Adapter: Invalid frame length");
      // Invalid length — reset
      frameState = FrameState::AwaitingLen1;
      frameLength = 0;
      frameIndex = 0;
    } else {
      frameIndex = 0;
      frameState = FrameState::AwaitingPayload;
    }
    break;

  case FrameState::AwaitingPayload:
    if (frameIndex >= MAX_PAYLOAD) {
      Logger::logln("SERIAL", "Frame parse error", LogLevel::LOG_WARN);
      lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER,
                         3, "Serial_Adapter: Frame buffer overflow");
      frameState = FrameState::AwaitingLen1;
      frameLength = 0;
      frameIndex = 0;
      break;
    }
    payloadBuffer[frameIndex++] = byteIn;
    if (frameIndex >= frameLength) {
      // Frame complete — leave frameIndex intact so frameLen() is valid until the next frame starts
      frameState = FrameState::AwaitingLen1;
      frameLength = 0;
      return true;
    }
    break;
  }
  return false;
}

} // namespace serial
} // namespace adapter
} // namespace lattice
