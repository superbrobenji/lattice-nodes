#include <gtest/gtest.h>
#include <cstring>
#include "mesh/serialization/mesh.pb.h"
#include "mesh/serialization/nanopb/pb_encode.h"
#include "mesh/serialization/nanopb/pb_decode.h"
#include "adapter/serial/SerialFraming.h"

using namespace lattice::adapter::serial;

// === SerialFraming tests ===

// Test encode → decode round-trip via SerialFraming high-level API
TEST(SerialFramingTest, EncodeDecodeRoundTrip) {
  lattice::mesh::mesh_message original{};
  original.message_type = MESH_TYPE_ADAPTER_DATA;
  memset(original.origin_mac_address, 0x11, 6);
  original.data[0] = 0x42;

  uint8_t buf[256];
  size_t len = SerialFraming::encode(original, buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  lattice::mesh::mesh_message decoded{};
  EXPECT_TRUE(SerialFraming::decode(buf, len, decoded));
  EXPECT_EQ(decoded.message_type, original.message_type);
  EXPECT_EQ(decoded.data[0], original.data[0]);
  // For ADAPTER_DATA the decode fills origin_mac_address with the device MAC (mock: AA:BB:CC:DD:EE:FF)
  const uint8_t mockMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  EXPECT_EQ(memcmp(decoded.origin_mac_address, mockMac, 6), 0);
}

// Test that decode() round-trips the full 64-byte data field (proto v2), not just
// the first 12 bytes (a stale proto-v1 cap left over from before data[12] -> data[64]).
TEST(SerialFramingTest, EncodeDecodeRoundTrip_Full64ByteData) {
  lattice::mesh::mesh_message original{};
  original.message_type = MESH_TYPE_ADAPTER_DATA;
  for (size_t i = 0; i < sizeof(original.data); ++i) {
    original.data[i] = static_cast<uint8_t>(i + 1); // 1..64, all non-zero
  }

  uint8_t buf[256];
  size_t len = SerialFraming::encode(original, buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  lattice::mesh::mesh_message decoded{};
  EXPECT_TRUE(SerialFraming::decode(buf, len, decoded));
  EXPECT_EQ(memcmp(decoded.data, original.data, sizeof(original.data)), 0)
      << "decode() must copy the full 64-byte data field, not truncate at 12 bytes";
}

// Test injectByte state machine: feed length-prefixed framed bytes one at a time
TEST(SerialFramingTest, InjectByteReassemblesFrame) {
  lattice::mesh::mesh_message original{};
  original.message_type = MESH_TYPE_MASTER_BEACON;

  // Encode the payload
  uint8_t payload[256];
  size_t payloadLen = SerialFraming::encode(original, payload, sizeof(payload));
  ASSERT_GT(payloadLen, 0u);

  // Build the length-prefixed frame: [len_lo][len_hi][payload...]
  uint8_t frame[258];
  frame[0] = static_cast<uint8_t>(payloadLen & 0xFF);
  frame[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  memcpy(frame + 2, payload, payloadLen);

  SerialFraming framing;
  bool complete = false;
  for (size_t i = 0; i < payloadLen + 2; ++i) {
    complete = framing.injectByte(frame[i]);
  }
  EXPECT_TRUE(complete);
  EXPECT_EQ(framing.frameLen(), payloadLen);

  lattice::mesh::mesh_message decoded{};
  EXPECT_TRUE(SerialFraming::decode(framing.frameBuffer(), framing.frameLen(), decoded));
  EXPECT_EQ(decoded.message_type, original.message_type);
}

// Test that injectByte correctly rejects frames with invalid length and recovers
TEST(SerialFramingTest, InjectByteRejectsInvalidLength) {
  SerialFraming framing;
  // Send length = 0 (invalid)
  EXPECT_FALSE(framing.injectByte(0x00)); // len_lo = 0
  EXPECT_FALSE(framing.injectByte(0x00)); // len_hi = 0 → invalid, resets to AwaitingLen1

  // Now send a valid frame of 1 byte
  EXPECT_FALSE(framing.injectByte(0x01)); // len_lo = 1
  EXPECT_FALSE(framing.injectByte(0x00)); // len_hi = 0 → length = 1
  EXPECT_TRUE(framing.injectByte(0xAB));  // payload byte → frame complete
  EXPECT_EQ(framing.frameLen(), 1u);
  EXPECT_EQ(framing.frameBuffer()[0], 0xAB);
}

// === Low-level nanopb codec tests (retained from original) ===

// Helper: encode a mesh_MeshMessage to a buffer, return bytes written (0 = failure)
static size_t encodeMsg(const mesh_MeshMessage& pbMsg, uint8_t* out, size_t outCap) {
  pb_ostream_t stream = pb_ostream_from_buffer(out, outCap);
  if (!pb_encode(&stream, mesh_MeshMessage_fields, &pbMsg)) return 0;
  return stream.bytes_written;
}

// Helper: decode bytes into a mesh_MeshMessage, return true on success
static bool decodeMsg(const uint8_t* data, size_t len, mesh_MeshMessage& pbMsg) {
  pbMsg = mesh_MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  return pb_decode(&stream, mesh_MeshMessage_fields, &pbMsg);
}

// --- ADAPTER_DATA round-trip ---
TEST(NanopbCodec, RoundTrip_AdapterData) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType  = 0;  // ADAPTER_DATA
  enc.dataType     = 0;  // PIR
  enc.hopCount     = 2;
  enc.epochNum     = 7;
  enc.seqNum       = 42;
  enc.protoVersion = 1;

  const uint8_t origin[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  memcpy(enc.originMacAddress, origin, 6);

  enc.has_data     = true;
  enc.data.size    = 12;
  for (int i = 0; i < 12; ++i) enc.data.bytes[i] = static_cast<uint8_t>(0x10 + i);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  mesh_MeshMessage dec;
  ASSERT_TRUE(decodeMsg(buf, n, dec));
  EXPECT_EQ(dec.messageType,  0u);
  EXPECT_EQ(dec.dataType,     0);
  EXPECT_EQ(dec.hopCount,     2u);
  EXPECT_EQ(dec.epochNum,     7u);
  EXPECT_EQ(dec.seqNum,       42u);
  EXPECT_EQ(dec.protoVersion, 1u);
  EXPECT_EQ(memcmp(dec.originMacAddress, origin, 6), 0);
  ASSERT_TRUE(dec.has_data);
  EXPECT_EQ(dec.data.size, 12u);
  for (int i = 0; i < 12; ++i) EXPECT_EQ(dec.data.bytes[i], 0x10 + i) << "data[" << i << "]";
  EXPECT_FALSE(dec.has_public_key);
}

// --- ENROLLMENT with public key ---
TEST(NanopbCodec, RoundTrip_Enrollment_WithKey) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType  = 2;  // ENROLLMENT
  enc.protoVersion = 1;

  const uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  memcpy(enc.originMacAddress, mac, 6);

  enc.has_public_key      = true;
  enc.public_key.size     = 32;
  for (int i = 0; i < 32; ++i) enc.public_key.bytes[i] = static_cast<uint8_t>(i + 1);

  enc.has_data  = true;
  enc.data.size = 12;
  memset(enc.data.bytes, 0, 12);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  mesh_MeshMessage dec;
  ASSERT_TRUE(decodeMsg(buf, n, dec));
  EXPECT_EQ(dec.messageType,  2u);
  EXPECT_EQ(dec.protoVersion, 1u);
  EXPECT_EQ(memcmp(dec.originMacAddress, mac, 6), 0);
  ASSERT_TRUE(dec.has_public_key);
  EXPECT_EQ(dec.public_key.size, 32u);
  for (int i = 0; i < 32; ++i) EXPECT_EQ(dec.public_key.bytes[i], i + 1) << "key[" << i << "]";
}

// --- JOIN_ACK with no key = rejection ---
TEST(NanopbCodec, RoundTrip_JoinAck_Rejected) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType  = 4;  // JOIN_ACK
  enc.protoVersion = 1;

  const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  memcpy(enc.originMacAddress, mac, 6);
  enc.has_public_key = false;  // no key = rejection

  enc.has_data  = true;
  enc.data.size = 12;
  memset(enc.data.bytes, 0, 12);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  mesh_MeshMessage dec;
  ASSERT_TRUE(decodeMsg(buf, n, dec));
  EXPECT_EQ(dec.messageType, 4u);
  EXPECT_EQ(memcmp(dec.originMacAddress, mac, 6), 0);
  EXPECT_FALSE(dec.has_public_key);
}

// --- Truncated buffer → decode fails ---
TEST(NanopbCodec, TruncatedBuffer_DecodeFails) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType = 0;
  enc.has_data    = true;
  enc.data.size   = 12;
  memset(enc.data.bytes, 0xAB, 12);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 4u);  // must have some bytes to truncate

  mesh_MeshMessage dec;
  // Provide only half the bytes
  EXPECT_FALSE(decodeMsg(buf, n / 2, dec));
}

// --- Empty buffer → decodes to zero-value (proto3: empty wire = all defaults) ---
TEST(NanopbCodec, EmptyBuffer_DecodesAsZeroMessage) {
  mesh_MeshMessage dec;
  // nanopb treats an empty stream as a valid all-zeros message (proto3 semantics)
  EXPECT_TRUE(decodeMsg(nullptr, 0, dec));
  EXPECT_EQ(dec.messageType, 0u);
  EXPECT_EQ(dec.dataType, 0);
  EXPECT_FALSE(dec.has_data);
  EXPECT_FALSE(dec.has_public_key);
}

// --- JOIN_ACK decode: originMacAddress must be preserved from the wire, not clobbered ---
// This is the precondition for the C1 fix: the codec must faithfully round-trip
// the enrolling node's MAC so that handleCompleteFrame → enrollPeer gets the
// correct MAC and does not register the master as its own peer.
TEST(NanopbCodec, JoinAckDecode_PreservesOriginMac) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType  = 4;  // JOIN_ACK
  enc.protoVersion = 1;

  const uint8_t nodeMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  memcpy(enc.originMacAddress, nodeMac, 6);

  enc.has_public_key    = true;
  enc.public_key.size   = 32;
  memset(enc.public_key.bytes, 0xAB, 32);

  enc.has_data  = true;
  enc.data.size = 12;
  memset(enc.data.bytes, 0, 12);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  mesh_MeshMessage dec;
  ASSERT_TRUE(decodeMsg(buf, n, dec));

  EXPECT_EQ(dec.messageType, 4u) << "messageType must survive round-trip";
  EXPECT_EQ(memcmp(dec.originMacAddress, nodeMac, 6), 0)
      << "JOIN_ACK originMacAddress must match the enrolling node MAC on the wire, "
         "not the master device's own MAC";
  ASSERT_TRUE(dec.has_public_key);
  EXPECT_EQ(dec.public_key.size, 32u);
  for (int i = 0; i < 32; ++i)
    EXPECT_EQ(dec.public_key.bytes[i], 0xAB) << "public_key.bytes[" << i << "]";
}

// --- sint32 zigzag: negative value survives round-trip (UNKNOWN_ADAPTER is now 0) ---
TEST(NanopbCodec, ZigZag_UnknownAdapter) {
  mesh_MeshMessage enc = mesh_MeshMessage_init_zero;
  enc.messageType = 0;
  enc.dataType    = -1;  // test negative sint32 zigzag encoding
  enc.has_data    = true;
  enc.data.size   = 12;
  memset(enc.data.bytes, 0, 12);

  uint8_t buf[256];
  size_t n = encodeMsg(enc, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  mesh_MeshMessage dec;
  ASSERT_TRUE(decodeMsg(buf, n, dec));
  EXPECT_EQ(dec.dataType, -1);
}
