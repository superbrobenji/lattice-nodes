#include "ProtobufCodec.h"
#include <cstring>
#include "src/core/Logger.h"

namespace planetopia {
namespace utils {

// Forward declaration of helper used before definition
static bool skipField(const uint8_t*& ptr, const uint8_t* end, uint8_t wireType);

size_t ProtobufCodec::encodeMeshMessage(const planetopia::mesh::mesh_message& msg,
                                        uint8_t* out, size_t outCap) {
  if (!out || outCap == 0) {
    logEncodeError("Invalid output buffer");
    return 0;
  }

  size_t idx = 0;

  // Field 1: messageType (uint32)
  if (idx + getVarintSize(static_cast<uint32_t>(msg.messageType)) > outCap) {
    logEncodeError("Buffer overflow during messageType encoding");
    return 0;
  }
  idx += writeUint32Field(out + idx, 1, static_cast<uint32_t>(msg.messageType));

  // Field 2: dataType (uint32)
  if (idx + getVarintSize(static_cast<uint32_t>(msg.dataType)) > outCap) {
    logEncodeError("Buffer overflow during dataType encoding");
    return 0;
  }
  idx += writeUint32Field(out + idx, 2, static_cast<uint32_t>(msg.dataType));

  // Field 3: targetMacAddress (bytes)
  if (idx + getVarintSize(6) + 6 > outCap) {
    logEncodeError("Buffer overflow during targetMacAddress encoding");
    return 0;
  }
  idx += writeBytesField(out + idx, 3, msg.targetMacAddress, 6);

  // Field 4: data (bytes)
  if (idx + getVarintSize(12) + 12 > outCap) {
    logEncodeError("Buffer overflow during data encoding");
    return 0;
  }
  idx += writeBytesField(out + idx, 4, msg.data, 12);

  LOG_D("CODEC", "Encode %u bytes", (unsigned)idx);
  return idx;
}

bool ProtobufCodec::decodeMeshMessage(const uint8_t* data, size_t len,
                                      planetopia::mesh::mesh_message& outMsg) {
  if (!data || len == 0) {
    logDecodeError("Invalid input data");
    return false;
  }

  // Initialize output message
  memset(&outMsg, 0, sizeof(outMsg));

  const uint8_t* ptr = data;
  const uint8_t* end = data + len;

  LOG_D("CODEC", "Decode %u bytes", (unsigned)len);

  // Auto-generate routing fields (as per protocol simplification)
  uint8_t ownMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, ownMac);

  memcpy(outMsg.originMacAddress, ownMac, 6);
  outMsg.hopCount = 0;
  memcpy(outMsg.lastHopMacAddress, ownMac, 6);

  while (ptr < end) {
    uint32_t fieldNumber;
    uint8_t wireType;

    if (!readKey(ptr, end, fieldNumber, wireType)) {
      logDecodeError("Failed to read field key");
      return false;
    }

    switch (fieldNumber) {
      case 1:  // messageType
        if (wireType != static_cast<uint8_t>(WireType::VARINT)) {
          logDecodeError("Wrong wire type for messageType");
          return false;
        }
        uint32_t msgType;
        if (!readVarint(ptr, end, msgType)) {
          logDecodeError("Failed to read messageType");
          return false;
        }
        outMsg.messageType = static_cast<planetopia::mesh::MeshMessageType>(msgType);
        break;

      case 2:  // dataType
        if (wireType != static_cast<uint8_t>(WireType::VARINT)) {
          logDecodeError("Wrong wire type for dataType");
          return false;
        }
        uint32_t dataType;
        if (!readVarint(ptr, end, dataType)) {
          logDecodeError("Failed to read dataType");
          return false;
        }
        outMsg.dataType = static_cast<planetopia::adapter::adapter_types>(dataType);
        break;

      case 3:  // targetMacAddress
        if (wireType != static_cast<uint8_t>(WireType::LENGTH_DELIMITED)) {
          logDecodeError("Wrong wire type for targetMacAddress");
          return false;
        }
        const uint8_t* macData;
        size_t macLen;
        if (!readLengthDelimited(ptr, end, macData, macLen)) {
          logDecodeError("Failed to read targetMacAddress");
          return false;
        }
        if (macLen != 6) {
          logDecodeError("Invalid MAC address length");
          return false;
        }
        memcpy(outMsg.targetMacAddress, macData, 6);
        break;

      case 4:  // data
        if (wireType != static_cast<uint8_t>(WireType::LENGTH_DELIMITED)) {
          logDecodeError("Wrong wire type for data");
          return false;
        }
        const uint8_t* dataPtr;
        size_t dataLen;
        if (!readLengthDelimited(ptr, end, dataPtr, dataLen)) {
          logDecodeError("Failed to read data");
          return false;
        }
        if (dataLen > 12) {
          logDecodeError("Data field too long");
          return false;
        }
        memcpy(outMsg.data, dataPtr, dataLen);
        break;

      default:
        // Skip unknown fields
        if (!skipField(ptr, end, wireType)) {
          logDecodeError("Failed to skip unknown field");
          return false;
        }
        break;
    }
  }

  LOG_D("CODEC", "Decode ok type=%u", (unsigned)outMsg.messageType);
  return true;
}

// Field encoding helpers
size_t ProtobufCodec::writeVarint(uint8_t* out, uint32_t value) {
  size_t idx = 0;
  while (value >= 0x80) {
    out[idx++] = static_cast<uint8_t>(value | 0x80);
    value >>= 7;
  }
  out[idx++] = static_cast<uint8_t>(value);
  return idx;
}

size_t ProtobufCodec::writeZigZag32(uint8_t* out, int32_t value) {
  uint32_t zigzag = (value << 1) ^ (value >> 31);
  return writeVarint(out, zigzag);
}

size_t ProtobufCodec::writeKey(uint8_t* out, uint32_t fieldNumber, uint8_t wireType) {
  uint32_t key = (fieldNumber << 3) | wireType;
  return writeVarint(out, key);
}

size_t ProtobufCodec::writeBytesField(uint8_t* out, uint32_t fieldNumber,
                                      const uint8_t* data, size_t len) {
  size_t idx = 0;
  idx += writeKey(out, fieldNumber, static_cast<uint8_t>(WireType::LENGTH_DELIMITED));
  idx += writeVarint(out + idx, len);
  memcpy(out + idx, data, len);
  idx += len;
  return idx;
}

size_t ProtobufCodec::writeSint32Field(uint8_t* out, uint32_t fieldNumber, int32_t value) {
  size_t idx = 0;
  idx += writeKey(out, fieldNumber, static_cast<uint8_t>(WireType::VARINT));
  idx += writeZigZag32(out + idx, value);
  return idx;
}

size_t ProtobufCodec::writeUint32Field(uint8_t* out, uint32_t fieldNumber, uint32_t value) {
  size_t idx = 0;
  idx += writeKey(out, fieldNumber, static_cast<uint8_t>(WireType::VARINT));
  idx += writeVarint(out + idx, value);
  return idx;
}

// Field decoding helpers
bool ProtobufCodec::readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out) {
  if (!checkBufferBounds(ptr, end, 1)) {
    return false;
  }

  uint32_t result = 0;
  int shift = 0;

  while (ptr < end) {
    uint8_t byte = *ptr++;
    result |= static_cast<uint32_t>(byte & 0x7F) << shift;

    if ((byte & 0x80) == 0) {
      out = result;
      return true;
    }

    shift += 7;
    if (shift >= 32) {
      logDecodeError("Varint too large");
      return false;
    }
  }

  logDecodeError("Unexpected end of buffer during varint read");
  return false;
}

bool ProtobufCodec::readZigZag32(const uint8_t*& ptr, const uint8_t* end, int32_t& out) {
  uint32_t zigzag;
  if (!readVarint(ptr, end, zigzag)) {
    return false;
  }

  out = static_cast<int32_t>((zigzag >> 1) ^ (-(zigzag & 1)));
  return true;
}

bool ProtobufCodec::readKey(const uint8_t*& ptr, const uint8_t* end,
                            uint32_t& fieldNumber, uint8_t& wireType) {
  uint32_t key;
  if (!readVarint(ptr, end, key)) {
    return false;
  }

  fieldNumber = key >> 3;
  wireType = key & 0x07;

  if (!isValidWireType(wireType)) {
    String msgWire = String(wireType);
    logDecodeError("Invalid wire type", msgWire.c_str());
    return false;
  }

  return true;
}

bool ProtobufCodec::readLengthDelimited(const uint8_t*& ptr, const uint8_t* end,
                                        const uint8_t*& dataPtr, size_t& dataLen) {
  uint32_t length;
  if (!readVarint(ptr, end, length)) {
    return false;
  }

  if (!checkBufferBounds(ptr, end, length)) {
    logDecodeError("Length-delimited field exceeds buffer bounds");
    return false;
  }

  dataPtr = ptr;
  dataLen = length;
  ptr += length;

  return true;
}

// Utility methods
bool ProtobufCodec::isValidWireType(uint8_t wireType) {
  return wireType <= static_cast<uint8_t>(WireType::FIXED32);
}

String ProtobufCodec::wireTypeToString(uint8_t wireType) {
  switch (static_cast<WireType>(wireType)) {
    case WireType::VARINT: return "VARINT";
    case WireType::FIXED64: return "FIXED64";
    case WireType::LENGTH_DELIMITED: return "LENGTH_DELIMITED";
    case WireType::START_GROUP: return "START_GROUP";
    case WireType::END_GROUP: return "END_GROUP";
    case WireType::FIXED32: return "FIXED32";
    default: return "UNKNOWN";
  }
}

size_t ProtobufCodec::getVarintSize(uint32_t value) {
  size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    size++;
  }
  return size;
}

size_t ProtobufCodec::getZigZag32Size(int32_t value) {
  uint32_t zigzag = (value << 1) ^ (value >> 31);
  return getVarintSize(zigzag);
}

// Private helper methods
bool ProtobufCodec::checkBufferBounds(const uint8_t* ptr, const uint8_t* end, size_t required) {
  if (ptr + required > end) {
    logDecodeError("Buffer overflow");
    return false;
  }
  return true;
}

void ProtobufCodec::logDecodeError(const char* operation, const char* details) {
  if (details) {
    Logger::logln("PROTOBUF", String("Decode error: ") + operation + " - " + details, LogLevel::LOG_ERROR);
  } else {
    Logger::logln("PROTOBUF", String("Decode error: ") + operation, LogLevel::LOG_ERROR);
  }
}

void ProtobufCodec::logEncodeError(const char* operation, const char* details) {
  if (details) {
    Logger::logln("PROTOBUF", String("Encode error: ") + operation + " - " + details, LogLevel::LOG_ERROR);
  } else {
    Logger::logln("PROTOBUF", String("Encode error: ") + operation, LogLevel::LOG_ERROR);
  }
}

// Helper function for skipping unknown fields
static bool skipField(const uint8_t*& ptr, const uint8_t* end, uint8_t wireType) {
  switch (static_cast<WireType>(wireType)) {
    case WireType::VARINT:
      {
        uint32_t dummy;
        return ProtobufCodec::readVarint(ptr, end, dummy);
      }
    case WireType::FIXED64:
      if (ptr + 8 > end) return false;
      ptr += 8;
      return true;
    case WireType::LENGTH_DELIMITED:
      {
        const uint8_t* dummy;
        size_t dummyLen;
        return ProtobufCodec::readLengthDelimited(ptr, end, dummy, dummyLen);
      }
    case WireType::FIXED32:
      if (ptr + 4 > end) return false;
      ptr += 4;
      return true;
    default:
      return false;
  }
}

}  // namespace utils
}  // namespace planetopia
