#ifndef PROTOBUF_CODEC_H
#define PROTOBUF_CODEC_H

#include <Arduino.h>
#include "src/Mesh/Mesh.h"
#include "src/core/Logger.h"

namespace planetopia {
namespace utils {

// Wire types for protobuf encoding
enum class WireType : uint8_t {
  VARINT = 0,
  FIXED64 = 1,
  LENGTH_DELIMITED = 2,
  START_GROUP = 3,
  END_GROUP = 4,
  FIXED32 = 5
};

class ProtobufCodec {
public:
  // Encoding methods
  static size_t encodeMeshMessage(const planetopia::mesh::mesh_message& msg,
                                  uint8_t* out, size_t outCap);

  // Decoding methods
  static bool decodeMeshMessage(const uint8_t* data, size_t len,
                                planetopia::mesh::mesh_message& outMsg);

  // Field encoding helpers
  static size_t writeVarint(uint8_t* out, uint32_t value);
  static size_t writeZigZag32(uint8_t* out, int32_t value);
  static size_t writeKey(uint8_t* out, uint32_t fieldNumber, uint8_t wireType);
  static size_t writeBytesField(uint8_t* out, uint32_t fieldNumber,
                                const uint8_t* data, size_t len);
  static size_t writeSint32Field(uint8_t* out, uint32_t fieldNumber, int32_t value);
  static size_t writeUint32Field(uint8_t* out, uint32_t fieldNumber, uint32_t value);

  // Field decoding helpers
  static bool readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out);
  static bool readZigZag32(const uint8_t*& ptr, const uint8_t* end, int32_t& out);
  static bool readKey(const uint8_t*& ptr, const uint8_t* end,
                      uint32_t& fieldNumber, uint8_t& wireType);
  static bool readLengthDelimited(const uint8_t*& ptr, const uint8_t* end,
                                  const uint8_t*& dataPtr, size_t& dataLen);

  // Utility methods
  static bool isValidWireType(uint8_t wireType);
  static String wireTypeToString(uint8_t wireType);
  static size_t getVarintSize(uint32_t value);
  static size_t getZigZag32Size(int32_t value);

private:
  // Internal constants
  static constexpr size_t MAX_FIELD_SIZE = 64;
  static constexpr size_t MAX_NESTED_LEVELS = 10;

  // Internal helper methods
  static bool checkBufferBounds(const uint8_t* ptr, const uint8_t* end, size_t required);
  static void logDecodeError(const char* operation, const char* details = nullptr);
  static void logEncodeError(const char* operation, const char* details = nullptr);
};

}  // namespace utils
}  // namespace planetopia

#endif  // PROTOBUF_CODEC_H
