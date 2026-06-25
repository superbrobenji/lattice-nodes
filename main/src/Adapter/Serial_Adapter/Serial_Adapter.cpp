#include "Serial_Adapter.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"
#include <esp_wifi.h>
#include "src/Mesh/Mesh.h"
#include <cstring>

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

uint32_t Serial_Adapter::lastHealthMillis = 0;

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

void Serial_Adapter::sendHealthReport() {
  Logger::logln("Serial_Adapter", "Sending health report", LogLevel::LOG_INFO);

  uint8_t data[12] = { 0 };
  data[0] = OP_HEALTH_REPORT;
  data[1] = static_cast<uint8_t>(SERIAL_ADAPTER);

  uint8_t mac[6];
  readOwnMac(mac);
  memcpy(&data[2], mac, 6);

  uint32_t uptimeSec = millis() / 1000;
  data[8] = static_cast<uint8_t>(uptimeSec & 0xFF);
  data[9] = static_cast<uint8_t>((uptimeSec >> 8) & 0xFF);
  data[10] = static_cast<uint8_t>((uptimeSec >> 16) & 0xFF);
  data[11] = static_cast<uint8_t>((uptimeSec >> 24) & 0xFF);

  Logger::logln("Serial_Adapter", String("Health report - MAC: ") + String(mac[0], HEX) + ":" + String(mac[1], HEX) + ":" + String(mac[2], HEX) + ":" + String(mac[3], HEX) + ":" + String(mac[4], HEX) + ":" + String(mac[5], HEX) + " Uptime: " + String(uptimeSec) + "s", LogLevel::LOG_DEBUG);

  if (planetopia::mesh::Mesh::transmit) {
    planetopia::mesh::Mesh::transmit(SERIAL_ADAPTER, data);
    Logger::logln("Serial_Adapter", "Health report sent via mesh", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Mesh transmit function not available for health report", LogLevel::LOG_WARN);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         1,
                         "Serial_Adapter: Mesh transmit not available");
  }
}

Serial_Adapter::Serial_Adapter(int pin)
  : Adapter(pin), frameState(FrameState::AwaitingLen1), frameLength(0), frameIndex(0) {
  _adapterType = SERIAL_ADAPTER;
  memset(payloadBuffer, 0, sizeof(payloadBuffer));

  Logger::logln("Serial_Adapter", "Serial_Adapter constructed with pin " + String(pin), LogLevel::LOG_INFO);
}

bool Serial_Adapter::init() {
  // Serial already initialized in main. Nothing to do.
  Logger::logln("Serial_Adapter", "Serial_Adapter initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void Serial_Adapter::loop() {
  // periodic health
  if (millis() - lastHealthMillis > 5000) {
    lastHealthMillis = static_cast<uint32_t>(millis());
    Logger::logln("Serial_Adapter", "Sending periodic health report", LogLevel::LOG_DEBUG);
    sendHealthReport();
  }

  while (Serial.available() > 0) {
    uint8_t byteIn = static_cast<uint8_t>(Serial.read());

    switch (frameState) {
      case FrameState::AwaitingLen1:
        frameLength = byteIn;
        Logger::logln("Serial_Adapter", "Received length byte 1: " + String(frameLength), LogLevel::LOG_DEBUG);
        frameState = FrameState::AwaitingLen2;
        break;

      case FrameState::AwaitingLen2:
        frameLength |= static_cast<uint16_t>(byteIn) << 8;
        Logger::logln("Serial_Adapter", "Received length byte 2, total length: " + String(frameLength), LogLevel::LOG_DEBUG);

        if (frameLength == 0 || frameLength > MAX_PAYLOAD) {
          Logger::logln("Serial_Adapter", "Invalid frame length: " + String(frameLength) + ", resetting frame state", LogLevel::LOG_WARN);
          planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                               planetopia::core::ModuleDigit::ADAPTER,
                               2,
                               "Serial_Adapter: Invalid frame length");
          // Reset on invalid length
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
        } else {
          frameIndex = 0;
          frameState = FrameState::AwaitingPayload;
          Logger::logln("Serial_Adapter", "Frame length valid, awaiting " + String(frameLength) + " payload bytes", LogLevel::LOG_DEBUG);
        }
        break;

      case FrameState::AwaitingPayload:
        if (frameIndex >= MAX_PAYLOAD) {
          Logger::logln("Serial_Adapter", "Frame buffer overflow, resetting frame state", LogLevel::LOG_ERROR);
          planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                               planetopia::core::ModuleDigit::ADAPTER,
                               3,
                               "Serial_Adapter: Frame buffer overflow");
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
          break;
        }

        payloadBuffer[frameIndex++] = byteIn;
        Logger::logln("Serial_Adapter", "Received payload byte " + String(frameIndex) + "/" + String(frameLength) + ": 0x" + String(byteIn, HEX), LogLevel::LOG_DEBUG);

        if (frameIndex >= frameLength) {
          Logger::logln("Serial_Adapter", "Frame complete, processing " + String(frameLength) + " bytes", LogLevel::LOG_DEBUG);
          handleCompleteFrame(payloadBuffer, frameLength);
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
        }
        break;
    }
  }
}

void Serial_Adapter::onMeshDataImpl(const planetopia::mesh::mesh_message& message) {
  Logger::logln("Serial_Adapter", "Processing incoming mesh message - Type: " + String(message.messageType) + " DataType: " + String(message.dataType) + " HopCount: " + String(message.hopCount), LogLevel::LOG_DEBUG);

  // Handle control opcodes received via mesh.
  // NOTE: OP_CONFIG_SET is now handled in Adapter::onMeshData() (base class) so it reaches
  // ALL node types. Only Serial_Adapter-specific opcodes remain here.
  if (message.dataType == SERIAL_ADAPTER) {
    uint8_t op = message.data[0];
    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request via mesh, sending health report", LogLevel::LOG_INFO);
      sendHealthReport();
    }
  }

  // Forward message to server via serial (existing encoding logic)
  uint8_t encoded[MAX_PAYLOAD];
  size_t n = encodeMeshMessage(message, encoded, sizeof(encoded));

  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode mesh message for serial output", LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         4,
                         "Serial_Adapter: Message encoding failed");
    return;
  }

  Logger::logln("Serial_Adapter", "Encoded mesh message to " + String(n) + " bytes, sending to serial", LogLevel::LOG_DEBUG);

  // 2-byte little-endian length prefix
  uint8_t lenLE[2] = { static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF) };
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);

  Logger::logln("Serial_Adapter", "Mesh message sent to serial successfully", LogLevel::LOG_DEBUG);
}

// --- Minimal Protobuf encoding helpers ---
size_t Serial_Adapter::writeVarint(uint8_t* out, uint32_t value) {
  size_t i = 0;
  while (value >= 0x80) {
    out[i++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  out[i++] = static_cast<uint8_t>(value);
  return i;
}

size_t Serial_Adapter::writeZigZag32(uint8_t* out, int32_t value) {
  uint32_t zigzag = (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
  return writeVarint(out, zigzag);
}

size_t Serial_Adapter::writeKey(uint8_t* out, uint32_t fieldNumber, uint8_t wireType) {
  return writeVarint(out, (fieldNumber << 3) | (wireType & 0x07));
}

size_t Serial_Adapter::writeBytesField(uint8_t* out, uint32_t fieldNumber, const uint8_t* data, size_t len) {
  size_t idx = 0;
  idx += writeKey(out + idx, fieldNumber, 2);
  idx += writeVarint(out + idx, static_cast<uint32_t>(len));
  memcpy(out + idx, data, len);
  idx += len;
  return idx;
}

size_t Serial_Adapter::writeSint32Field(uint8_t* out, uint32_t fieldNumber, int32_t value) {
  size_t idx = 0;
  idx += writeKey(out + idx, fieldNumber, 0);
  idx += writeZigZag32(out + idx, value);
  return idx;
}

size_t Serial_Adapter::writeUint32Field(uint8_t* out, uint32_t fieldNumber, uint32_t value) {
  size_t idx = 0;
  idx += writeKey(out + idx, fieldNumber, 0);
  idx += writeVarint(out + idx, value);
  return idx;
}

bool Serial_Adapter::readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out) {
  uint32_t result = 0;
  int shift = 0;
  while (ptr < end && shift <= 28) {
    uint8_t byte = *ptr++;
    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      out = result;
      return true;
    }
    shift += 7;
  }
  Logger::logln("Serial_Adapter", "Varint read failed - buffer overflow or malformed data", LogLevel::LOG_WARN);
  return false;
}

bool Serial_Adapter::readZigZag32(const uint8_t*& ptr, const uint8_t* end, int32_t& out) {
  uint32_t u = 0;
  if (!readVarint(ptr, end, u)) {
    Logger::logln("Serial_Adapter", "ZigZag32 read failed - varint read failed", LogLevel::LOG_WARN);
    return false;
  }
  out = static_cast<int32_t>((u >> 1) ^ (~(u & 1) + 1));
  return true;
}

bool Serial_Adapter::readKey(const uint8_t*& ptr, const uint8_t* end, uint32_t& fieldNumber, uint8_t& wireType) {
  uint32_t key = 0;
  if (!readVarint(ptr, end, key)) {
    Logger::logln("Serial_Adapter", "Key read failed - varint read failed", LogLevel::LOG_WARN);
    return false;
  }
  wireType = static_cast<uint8_t>(key & 0x07);
  fieldNumber = key >> 3;
  return true;
}

bool Serial_Adapter::readLengthDelimited(const uint8_t*& ptr, const uint8_t* end, const uint8_t*& dataPtr, size_t& dataLen) {
  uint32_t len = 0;
  if (!readVarint(ptr, end, len)) {
    Logger::logln("Serial_Adapter", "Length read failed - varint read failed", LogLevel::LOG_WARN);
    return false;
  }
  if (ptr + len > end) {
    Logger::logln("Serial_Adapter", "Length-delimited field exceeds buffer bounds", LogLevel::LOG_WARN);
    return false;
  }
  dataPtr = ptr;
  dataLen = len;
  ptr += len;
  return true;
}

size_t Serial_Adapter::encodeMeshMessage(const planetopia::mesh::mesh_message& msg, uint8_t* out, size_t outCap) {
  Logger::logln("Serial_Adapter", "Encoding mesh message - Type: " + String(msg.messageType) + " DataType: " + String(msg.dataType) + " HopCount: " + String(msg.hopCount), LogLevel::LOG_DEBUG);

  size_t idx = 0;
  auto ensure = [&](size_t need) {
    return idx + need <= outCap;
  };

  uint8_t tmp[32];
  size_t n;

  // 1: messageType (uint32 varint)
  n = writeUint32Field(tmp, 1, static_cast<uint32_t>(msg.messageType));
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding messageType", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  // 2: dataType (sint32 zigzag)
  n = writeSint32Field(tmp, 2, static_cast<int32_t>(msg.dataType));
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding dataType", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  // 3,4,5: MACs as bytes (len 6)
  n = writeBytesField(tmp, 3, msg.originMacAddress, 6);
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding originMacAddress", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  n = writeBytesField(tmp, 4, msg.targetMacAddress, 6);
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding targetMacAddress", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  n = writeBytesField(tmp, 5, msg.lastHopMacAddress, 6);
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding lastHopMacAddress", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  // 6: data (len 12)
  n = writeBytesField(tmp, 6, msg.data, 12);
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding data", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  // 7: hopCount (uint32 varint)
  n = writeUint32Field(tmp, 7, static_cast<uint32_t>(msg.hopCount));
  if (!ensure(n)) {
    Logger::logln("Serial_Adapter", "Buffer overflow while encoding hopCount", LogLevel::LOG_ERROR);
    return 0;
  }
  memcpy(out + idx, tmp, n);
  idx += n;

  Logger::logln("Serial_Adapter", "Successfully encoded mesh message to " + String(idx) + " bytes", LogLevel::LOG_DEBUG);
  return idx;
}

bool Serial_Adapter::decodeMeshMessage(const uint8_t* data, size_t len, planetopia::mesh::mesh_message& outMsg) {
  Logger::logln("Serial_Adapter", "Decoding protobuf message of " + String(len) + " bytes", LogLevel::LOG_DEBUG);

  memset(&outMsg, 0, sizeof(outMsg));
  const uint8_t* ptr = data;
  const uint8_t* end = data + len;

  // Auto-generate routing fields that the server doesn't need to send
  // This simplifies the server implementation - it only needs to send essential fields
  readOwnMac(outMsg.originMacAddress);  // Set origin to this node's MAC
  outMsg.hopCount = 0;                  // Start at 0 hops for serial-originated messages

  Logger::logln("Serial_Adapter", "Auto-generated routing fields - Origin MAC: " + String(outMsg.originMacAddress[0], HEX) + ":" + String(outMsg.originMacAddress[1], HEX) + ":" + String(outMsg.originMacAddress[2], HEX) + ":" + String(outMsg.originMacAddress[3], HEX) + ":" + String(outMsg.originMacAddress[4], HEX) + ":" + String(outMsg.originMacAddress[5], HEX) + " HopCount: " + String(outMsg.hopCount), LogLevel::LOG_DEBUG);

  while (ptr < end) {
    uint32_t field = 0;
    uint8_t wt = 0;
    if (!readKey(ptr, end, field, wt)) {
      Logger::logln("Serial_Adapter", "Failed to read protobuf field key", LogLevel::LOG_ERROR);
      return false;
    }
    if (field == 0) {
      Logger::logln("Serial_Adapter", "Invalid field number 0", LogLevel::LOG_ERROR);
      return false;
    }

    Logger::logln("Serial_Adapter", "Processing field " + String(field) + " with wire type " + String(wt), LogLevel::LOG_DEBUG);

    switch (field) {
      case 1:
        {  // messageType: varint
          if (wt != 0) {
            Logger::logln("Serial_Adapter", "messageType has wrong wire type: " + String(wt) + " (expected 0)", LogLevel::LOG_ERROR);
            return false;
          }
          uint32_t v = 0;
          if (!readVarint(ptr, end, v)) {
            Logger::logln("Serial_Adapter", "Failed to read messageType value", LogLevel::LOG_ERROR);
            return false;
          }
          outMsg.messageType = static_cast<planetopia::mesh::MeshMessageType>(v);
          Logger::logln("Serial_Adapter", "Decoded messageType: " + String(v), LogLevel::LOG_DEBUG);
          break;
        }
      case 2:
        {  // dataType: sint32
          if (wt != 0) {
            Logger::logln("Serial_Adapter", "dataType has wrong wire type: " + String(wt) + " (expected 0)", LogLevel::LOG_ERROR);
            return false;
          }
          int32_t v = 0;
          if (!readZigZag32(ptr, end, v)) {
            Logger::logln("Serial_Adapter", "Failed to read dataType value", LogLevel::LOG_ERROR);
            return false;
          }
          outMsg.dataType = static_cast<planetopia::adapter::adapter_types>(v);
          Logger::logln("Serial_Adapter", "Decoded dataType: " + String(v), LogLevel::LOG_DEBUG);
          break;
        }
      case 4:  // targetMacAddress (only field the server needs to send)
        {
          if (wt != 2) {
            Logger::logln("Serial_Adapter", "targetMacAddress has wrong wire type: " + String(wt) + " (expected 2)", LogLevel::LOG_ERROR);
            return false;
          }
          const uint8_t* p = nullptr;
          size_t l = 0;
          if (!readLengthDelimited(ptr, end, p, l)) {
            Logger::logln("Serial_Adapter", "Failed to read targetMacAddress length", LogLevel::LOG_ERROR);
            return false;
          }
          memset(outMsg.targetMacAddress, 0, 6);
          if (l > 0) memcpy(outMsg.targetMacAddress, p, l > 6 ? 6 : l);
          Logger::logln("Serial_Adapter", "Decoded targetMacAddress: " + String(outMsg.targetMacAddress[0], HEX) + ":" + String(outMsg.targetMacAddress[1], HEX) + ":" + String(outMsg.targetMacAddress[2], HEX) + ":" + String(outMsg.targetMacAddress[3], HEX) + ":" + String(outMsg.targetMacAddress[4], HEX) + ":" + String(outMsg.targetMacAddress[5], HEX), LogLevel::LOG_DEBUG);
          break;
        }
      case 6:
        {  // data
          if (wt != 2) {
            Logger::logln("Serial_Adapter", "data has wrong wire type: " + String(wt) + " (expected 2)", LogLevel::LOG_ERROR);
            return false;
          }
          const uint8_t* p = nullptr;
          size_t l = 0;
          if (!readLengthDelimited(ptr, end, p, l)) {
            Logger::logln("Serial_Adapter", "Failed to read data length", LogLevel::LOG_ERROR);
            return false;
          }
          memset(outMsg.data, 0, 12);
          if (l > 0) memcpy(outMsg.data, p, l > 12 ? 12 : l);
          Logger::logln("Serial_Adapter", "Decoded data payload of " + String(l) + " bytes", LogLevel::LOG_DEBUG);
          break;
        }
      default:
        {
          Logger::logln("Serial_Adapter", "Skipping unknown field " + String(field) + " with wire type " + String(wt), LogLevel::LOG_DEBUG);
          // Skip unknown fields (including 3: originMacAddress, 5: lastHopMacAddress, 7: hopCount)
          if (wt == 0) {
            uint32_t dummy;
            if (!readVarint(ptr, end, dummy)) {
              Logger::logln("Serial_Adapter", "Failed to skip varint field", LogLevel::LOG_ERROR);
              return false;
            }
          } else if (wt == 2) {
            const uint8_t* p;
            size_t l;
            if (!readLengthDelimited(ptr, end, p, l)) {
              Logger::logln("Serial_Adapter", "Failed to skip length-delimited field", LogLevel::LOG_ERROR);
              return false;
            }
          } else {
            Logger::logln("Serial_Adapter", "Unknown wire type: " + String(wt), LogLevel::LOG_ERROR);
            return false;
          }
          break;
        }
    }
  }

  // Set lastHopMacAddress to this node's MAC (since we're the origin)
  readOwnMac(outMsg.lastHopMacAddress);
  Logger::logln("Serial_Adapter", "Set lastHopMacAddress to own MAC: " + String(outMsg.lastHopMacAddress[0], HEX) + ":" + String(outMsg.lastHopMacAddress[1], HEX) + ":" + String(outMsg.lastHopMacAddress[2], HEX) + ":" + String(outMsg.lastHopMacAddress[3], HEX) + ":" + String(outMsg.lastHopMacAddress[4], HEX) + ":" + String(outMsg.lastHopMacAddress[5], HEX), LogLevel::LOG_DEBUG);

  Logger::logln("Serial_Adapter", "Successfully decoded protobuf message", LogLevel::LOG_DEBUG);
  return true;
}

void Serial_Adapter::relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]) {
  // Raw enrollment frame: [C0][6B mac][32B pubkey] = 39 bytes total
  uint8_t frame[39];
  frame[0] = OP_ENROLLMENT_REQ;
  memcpy(&frame[1], mac, 6);
  memcpy(&frame[7], pubKey, 32);

  // 2-byte little-endian length prefix (matches the framing protocol)
  uint8_t lenLE[2] = { static_cast<uint8_t>(39 & 0xFF), static_cast<uint8_t>((39 >> 8) & 0xFF) };
  Serial.write(lenLE, 2);
  Serial.write(frame, 39);
  Logger::logln("Serial_Adapter", "Enrollment request relayed to server", LogLevel::LOG_INFO);
}

void Serial_Adapter::handleCompleteFrame(const uint8_t* data, size_t len) {
  Logger::logln("Serial_Adapter", "Handling complete frame of " + String(len) + " bytes", LogLevel::LOG_INFO);

  // Check for raw enrollment opcodes before attempting protobuf decode
  if (len >= 1) {
    uint8_t op = data[0];
    if (op == OP_ENROLLMENT_APPROVE && len >= 39) {
      // Server approved: [C1][6B mac][32B pubkey]
      uint8_t approvedMac[6];
      uint8_t approvedPubKey[32];
      memcpy(approvedMac,    &data[1], 6);
      memcpy(approvedPubKey, &data[7], 32);
      Logger::logln("Serial_Adapter", "Server approved enrollment, registering peer", LogLevel::LOG_INFO);
      planetopia::mesh::Mesh* meshInstance = planetopia::mesh::Mesh::getInstance();
      if (meshInstance) {
        meshInstance->enrollPeer(approvedMac, approvedPubKey);
      }
      return;
    } else if (op == OP_ENROLLMENT_REJECT) {
      Logger::logln("Serial_Adapter", "Server rejected enrollment request", LogLevel::LOG_WARN);
      return;
    }
  }

  planetopia::mesh::mesh_message msg;
  if (!decodeMeshMessage(data, len, msg)) {
    Logger::logln("Serial_Adapter", "Failed to decode protobuf frame", LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         5,
                         "Serial_Adapter: Failed to decode protobuf frame");
    return;
  }

  Logger::logln("Serial_Adapter", "Decoded message - Type: " + String(msg.messageType) + " DataType: " + String(msg.dataType), LogLevel::LOG_INFO);

  // Only forward adapter data via mesh transmit function; routing fields are managed by Mesh
  if (msg.messageType == planetopia::mesh::MESH_TYPE_ADAPTER_DATA) {
    Logger::logln("Serial_Adapter", "Forwarding adapter data via mesh transmit", LogLevel::LOG_DEBUG);

    if (mesh_transmit_fn) {
      // Targeted send via normal mesh transmit path (to master, route onward)
      mesh_transmit_fn(msg.dataType, msg.data);
      Logger::logln("Serial_Adapter", "Adapter data forwarded successfully", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("Serial_Adapter", "transmit function not set", LogLevel::LOG_ERROR);
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::ADAPTER,
                           6,
                           "Serial_Adapter: transmit function not set");
    }
  } else if (msg.messageType == SERIAL_MSG_BROADCAST) {
    Logger::logln("Serial_Adapter", "Broadcasting adapter data to all peers", LogLevel::LOG_DEBUG);
    // Broadcast adapter data to all peers
    planetopia::mesh::Mesh::broadcastAdapterDataStatic(msg.dataType, msg.data);
    Logger::logln("Serial_Adapter", "Broadcast sent successfully", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Unknown message type: " + String(msg.messageType), LogLevel::LOG_WARN);
  }

  // Handle control opcodes: CONFIG_SET, HEALTH_REQ
  if (msg.dataType == SERIAL_ADAPTER) {
    uint8_t op = msg.data[0];
    Logger::logln("Serial_Adapter", "Processing SERIAL_ADAPTER control opcode: 0x" + String(op, HEX), LogLevel::LOG_DEBUG);

    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request, sending health report", LogLevel::LOG_INFO);
      sendHealthReport();
    } else if (op == OP_CONFIG_SET) {
      Logger::logln("Serial_Adapter", "Received configuration set request", LogLevel::LOG_INFO);

      // Apply only if targeted to me (or broadcast FF:..:FF)
      uint8_t myMac[6];
      readOwnMac(myMac);
      bool allFF = true;
      for (int i = 0; i < 6; ++i)
        if (msg.targetMacAddress[i] != 0xFF) {
          allFF = false;
          break;
        }
      bool isTarget = allFF || (memcmp(msg.targetMacAddress, myMac, 6) == 0);

      if (isTarget) {
        adapter_types newType = static_cast<adapter_types>(static_cast<int8_t>(msg.data[7]));
        Logger::logln("Serial_Adapter", "Configuration applies to this node, setting adapter type to: " + String(newType), LogLevel::LOG_INFO);

        planetopia::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        Logger::logln("Serial_Adapter", "Adapter type saved to EEPROM successfully", LogLevel::LOG_INFO);
        // Pin is automatically inferred from adapter type - no need to store it
        // Let main recreate adapter on next boot or we could soft-switch by signaling error-led+restart
        Logger::logln("Serial_Adapter", "Restarting device to apply new configuration", LogLevel::LOG_INFO);
        ESP.restart();
      } else {
        Logger::logln("Serial_Adapter", "Configuration not targeted to this node, ignoring", LogLevel::LOG_DEBUG);
      }
    } else {
      Logger::logln("Serial_Adapter", "Unknown SERIAL_ADAPTER opcode: 0x" + String(op, HEX), LogLevel::LOG_WARN);
    }
  }

  Logger::logln("Serial_Adapter", "Frame processing completed successfully", LogLevel::LOG_DEBUG);
}

}  // namespace adapter
}  // namespace planetopia
