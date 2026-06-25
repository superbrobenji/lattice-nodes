#include <gtest/gtest.h>
#include "Mesh/serialization/ProtobufCodec.h"
#include "Mesh/Mesh.h"  // for mesh_message struct

using planetopia::utils::ProtobufCodec;
using planetopia::mesh::mesh_message;
using planetopia::mesh::MeshMessageType;
using planetopia::adapter::adapter_types;

class ProtobufCodecTest : public ::testing::Test {
protected:
  // Thin wrappers that match the brief's expected API shape
  // (real codec uses static methods with 3-arg encode returning size_t)

  bool encodeMeshMessage(const mesh_message& msg, uint8_t* buf, size_t cap, size_t* outLen) {
    size_t n = ProtobufCodec::encodeMeshMessage(msg, buf, cap);
    if (n == 0) return false;
    *outLen = n;
    return true;
  }

  bool decodeMeshMessage(const uint8_t* buf, size_t len, mesh_message* out) {
    return ProtobufCodec::decodeMeshMessage(buf, len, *out);
  }

  mesh_message makeMsg(uint32_t type, int32_t dataType, const uint8_t origin[6]) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType  = static_cast<MeshMessageType>(type);
    m.dataType     = static_cast<adapter_types>(dataType);
    memcpy(m.originMacAddress, origin, 6);
    memset(m.targetMacAddress, 0xFF, 6);
    memset(m.lastHopMacAddress, 0x00, 6);
    m.hopCount = 2;
    m.epochNum = 7;
    m.seqNum   = 42;
    return m;
  }
};

TEST_F(ProtobufCodecTest, RoundTrip_AllFields) {
  const uint8_t origin[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  mesh_message original = makeMsg(0, 0, origin);
  original.data[0] = 0xB1;  // OP_HEALTH_REPORT
  original.data[1] = 42;    // uptime low byte

  uint8_t encoded[256];
  size_t  encodedLen = 0;
  ASSERT_TRUE(encodeMeshMessage(original, encoded, sizeof(encoded), &encodedLen));
  EXPECT_GT(encodedLen, 0u);

  mesh_message decoded{};
  ASSERT_TRUE(decodeMeshMessage(encoded, encodedLen, &decoded));

  // Codec encodes: messageType, dataType, targetMacAddress, data
  EXPECT_EQ(decoded.messageType,  original.messageType);
  EXPECT_EQ(decoded.dataType,     original.dataType);
  EXPECT_EQ(memcmp(decoded.targetMacAddress, original.targetMacAddress, 6), 0);
  EXPECT_EQ(decoded.data[0], 0xB1);
  EXPECT_EQ(decoded.data[1], 42);
}

TEST_F(ProtobufCodecTest, RoundTrip_NegativeDataType) {
  // UNKNOWN_ADAPTER = -1 must survive encoding as uint32 (0xFFFFFFFF) and cast back
  const uint8_t origin[6] = {};
  mesh_message m = makeMsg(1, -1, origin);

  uint8_t buf[256];
  size_t  len = 0;
  ASSERT_TRUE(encodeMeshMessage(m, buf, sizeof(buf), &len));

  mesh_message out{};
  ASSERT_TRUE(decodeMeshMessage(buf, len, &out));
  EXPECT_EQ(static_cast<int>(out.dataType), -1);
}

TEST_F(ProtobufCodecTest, Decode_TruncatedInput_ReturnsFalse) {
  const uint8_t origin[6] = {};
  mesh_message m = makeMsg(0, 0, origin);
  uint8_t buf[256];
  size_t  len = 0;
  ASSERT_TRUE(encodeMeshMessage(m, buf, sizeof(buf), &len));

  // Truncate to half
  mesh_message out{};
  EXPECT_FALSE(decodeMeshMessage(buf, len / 2, &out));
}

TEST_F(ProtobufCodecTest, Decode_EmptyInput_ReturnsFalse) {
  uint8_t buf[1] = {0};
  mesh_message out{};
  EXPECT_FALSE(decodeMeshMessage(buf, 0, &out));
}

TEST_F(ProtobufCodecTest, Encode_BufferTooSmall_ReturnsFalse) {
  const uint8_t origin[6] = {};
  mesh_message m = makeMsg(0, 0, origin);
  uint8_t buf[4];  // Too small
  size_t  len = 0;
  EXPECT_FALSE(encodeMeshMessage(m, buf, sizeof(buf), &len));
}

TEST_F(ProtobufCodecTest, RoundTrip_ZeroDataField) {
  const uint8_t origin[6] = {};
  mesh_message m = makeMsg(0, 0, origin);
  memset(m.data, 0, sizeof(m.data));

  uint8_t buf[256];
  size_t  len = 0;
  ASSERT_TRUE(encodeMeshMessage(m, buf, sizeof(buf), &len));

  mesh_message out{};
  ASSERT_TRUE(decodeMeshMessage(buf, len, &out));
  EXPECT_EQ(memcmp(out.data, m.data, sizeof(m.data)), 0);
}

TEST_F(ProtobufCodecTest, RoundTrip_MaxDataField) {
  const uint8_t origin[6] = {};
  mesh_message m = makeMsg(0, 0, origin);
  for (int i = 0; i < 12; ++i) m.data[i] = static_cast<uint8_t>(0xAA + i);

  uint8_t buf[256];
  size_t  len = 0;
  ASSERT_TRUE(encodeMeshMessage(m, buf, sizeof(buf), &len));

  mesh_message out{};
  ASSERT_TRUE(decodeMeshMessage(buf, len, &out));
  for (int i = 0; i < 12; ++i)
    EXPECT_EQ(out.data[i], m.data[i]) << "data[" << i << "] mismatch";
}
