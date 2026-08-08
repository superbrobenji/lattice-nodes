#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/CompactMessage.h"

using namespace lattice::mesh;

namespace {

// Fills every field of a wire mesh_message with distinct, non-zero, easily
// distinguishable bytes so a round-trip test can catch both "field dropped"
// and "field cross-wired" bugs.
mesh_message makeFullWireMessage() {
  mesh_message msg{};
  msg.proto_version = 0x04;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = 7;
  for (int i = 0; i < 6; ++i)
    msg.origin_mac_address[i] = static_cast<uint8_t>(0x10 + i);
  for (int i = 0; i < 6; ++i)
    msg.target_mac_address[i] = static_cast<uint8_t>(0x20 + i);
  for (int i = 0; i < 6; ++i)
    msg.last_hop_mac_address[i] = static_cast<uint8_t>(0x30 + i);
  for (int i = 0; i < 64; ++i)
    msg.data[i] = static_cast<uint8_t>(0x40 + (i % 64));
  msg.hop_count = 5;
  msg.epoch_num = 0xAABBCCDDu;
  msg.seq_num = 0xBEEF;
  for (int i = 0; i < 32; ++i)
    msg.enrollment_public_key[i] = static_cast<uint8_t>(0x50 + i);
  msg.route_len = 3;
  for (int i = 0; i < 48; ++i)
    msg.route_path[i] = static_cast<uint8_t>(0x60 + (i % 48));
  for (int i = 0; i < 16; ++i)
    msg.auth_tag[i] = static_cast<uint8_t>(0x70 + i);
  for (int i = 0; i < 8; ++i)
    msg.auth_path[i] = static_cast<uint8_t>(0xA0 + i);
  return msg;
}

} // namespace

// Every field CompactMessage declares must survive a wire -> compact -> wire
// round trip byte-for-byte.
TEST(CompactMessage, RoundTripPreservesStoredFields) {
  mesh_message src = makeFullWireMessage();

  CompactMessage compact{};
  toCompact(src, compact);

  mesh_message out{};
  toWire(compact, out);

  EXPECT_EQ(out.proto_version, src.proto_version);
  EXPECT_EQ(out.message_type, src.message_type);
  EXPECT_EQ(out.data_type, src.data_type);
  EXPECT_EQ(0, memcmp(out.origin_mac_address, src.origin_mac_address, 6));
  EXPECT_EQ(0, memcmp(out.target_mac_address, src.target_mac_address, 6));
  EXPECT_EQ(0, memcmp(out.last_hop_mac_address, src.last_hop_mac_address, 6));
  EXPECT_EQ(0, memcmp(out.data, src.data, sizeof(src.data)));
  EXPECT_EQ(out.hop_count, src.hop_count);
  EXPECT_EQ(out.epoch_num, src.epoch_num);
  EXPECT_EQ(out.seq_num, src.seq_num);
  EXPECT_EQ(0, memcmp(out.enrollment_public_key, src.enrollment_public_key, 32));
  EXPECT_EQ(out.route_len, src.route_len);
  EXPECT_EQ(0, memcmp(out.route_path, src.route_path, sizeof(src.route_path)));
  EXPECT_EQ(0, memcmp(out.auth_tag, src.auth_tag, sizeof(src.auth_tag)));
  EXPECT_EQ(0, memcmp(out.auth_path, src.auth_path, sizeof(src.auth_path)));
}

// NOTE: the old "RoundTripZeroesSkippedFields" test (secondary_master_mac /
// secondary_public_key dropped by CompactMessage) was removed here — protocol
// v0.6.0 (wire shrink §8) deleted those top-level mesh_message fields
// entirely, so there is nothing left for CompactMessage to skip; JOIN_ACK's
// secondary identity now lives in data[4..42], which round-trips as part of
// the ordinary `data` field coverage in RoundTripPreservesStoredFields above.

// toWire() fully overwrites its output — no stale data from a previous
// reconstruction should leak through (matters since MeshTransport::drain
// reuses a single stack RecvQueueEntry per iteration in MeshTransport.cpp).
TEST(CompactMessage, ToWireOverwritesPreviousContents) {
  mesh_message stale = makeFullWireMessage(); // fully non-zero, incl. dropped fields
  CompactMessage empty{}; // zero-initialized compact message

  toWire(empty, stale);

  mesh_message allZero{};
  EXPECT_EQ(0, memcmp(&stale, &allZero, sizeof(mesh_message)));
}

// Zero-valued messages (e.g. a freshly-constructed mesh_message{}) round-trip
// to zero — no field spuriously becomes non-zero.
TEST(CompactMessage, ZeroMessageRoundTripsToZero) {
  mesh_message src{};
  CompactMessage compact{};
  toCompact(src, compact);
  mesh_message out{};
  // Poison `out` first so a no-op toWire() couldn't accidentally pass by
  // leaving stale zero bytes untouched.
  memset(&out, 0xFF, sizeof(out));
  toWire(compact, out);

  mesh_message allZero{};
  EXPECT_EQ(0, memcmp(&out, &allZero, sizeof(mesh_message)));
}

// Protocol v0.6.0 (wire shrink §8) removed secondary_master_mac/
// secondary_public_key from mesh_message entirely — those were the only two
// fields CompactMessage ever dropped (see CompactMessage.h's rationale
// comment), so there is nothing left for it to shrink relative to the
// now-200B wire struct. This asserts CompactMessage never grows PAST the
// wire size (it still carries every field mesh_message has), not that it is
// smaller — that guarantee no longer holds now that its one differentiator
// was deleted upstream rather than dropped by CompactMessage itself.
TEST(CompactMessage, SizeDoesNotExceedWire) {
  EXPECT_LE(sizeof(CompactMessage), sizeof(mesh_message));
}
