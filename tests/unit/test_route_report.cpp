#include <gtest/gtest.h>
#include "mesh/Mesh.h"
#include "mesh/MeshCrypto.h"
#include "mesh/E2ECrypto.h"
#include "mesh/RouteMac.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "lib/lattice-protocol/c/opcodes.h"

using namespace lattice::mesh;

class RouteReportTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    // Task 6 (E2E AEAD): self-originated ADAPTER_DATA/ROUTE_REPORT uplinks are
    // now sealed in transmitCore(), which requires a real (non-zero) master
    // pubkey to derive a key with — generate real Curve25519 keypairs once per
    // test so setupRelayNode() below can register a properly keyed master peer.
    lattice::mesh::crypto::generateKeypair(myPriv, myPub);
    lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  }

  uint8_t myPriv[32], myPub[32];
  uint8_t masterPriv[32], masterPub[32];

  // Set up mesh as non-master with a reachable, E2E-keyed master peer.
  void setupRelayNode(Mesh& mesh, const uint8_t masterMac[6]) {
    static const uint8_t myMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    memcpy(mesh.deviceMacAddress, myMac, 6);
    mesh.isMaster = false;
    mesh.enrollment.hasMasterMac = true;
    memcpy(mesh.enrollment.knownMasterMac, masterMac, 6);
    memcpy(mesh.currentMaster.mac, masterMac, 6);
    mesh.currentMaster.distance = 1;
    memcpy(mesh.enrollment.devicePrivateKey, myPriv, 32);
    memcpy(mesh.enrollment.devicePublicKey, myPub, 32);

    // Register master as a live, keyed peer so findNextHopToMaster() AND E2E
    // sealing (lattice::mesh::masterE2EKeys) both succeed.
    PeerInfo peer{};
    memcpy(peer.mac, masterMac, 6);
    memcpy(peer.publicKey, masterPub, 32);
    peer.lastSeenMs = millis();
    mesh.peers.append(peer);
  }

  mesh_message lastSentMsg() {
    if (espNowSentPackets.empty()) {
      ADD_FAILURE() << "espNowSentPackets is empty — expected at least one sent packet";
      return {};
    }
    return *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  }

  // The k_up this node uses to seal uplinks to the master set up by
  // setupRelayNode() — ECDH is symmetric, so deriving from our own priv + the
  // master's pubkey matches what the master derives from its priv + our pubkey
  // (mirrors lattice::mesh::masterE2EKeys).
  void deriveUpKey(uint8_t kUpOut[32]) {
    uint8_t kDown[32];
    lattice::mesh::crypto::deriveE2EKeys(myPriv, masterPub, kUpOut, kDown);
  }

  // Chain-MAC (Phase C, spec §4 / issue #44) shared fixture: sets `mesh` up
  // as master with origin/r1/r2 all registered as keyed peers, and returns a
  // sealed, valid 2-hop route report (origin -> r1 -> r2 -> master) with a
  // correctly chained auth_path — the base case the tamper/unknown-hop tests
  // below mutate from.
  mesh_message buildValidTwoHopChain(Mesh& mesh, const uint8_t originMac[6],
                                     const uint8_t r1[6], const uint8_t r2[6]) {
    mesh.isMaster = true;
    mesh.reevaluateRouteTable();

    uint8_t originPriv[32], originPub[32];
    uint8_t r1Priv[32], r1Pub[32];
    uint8_t r2Priv[32], r2Pub[32];
    lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
    lattice::mesh::crypto::generateKeypair(originPriv, originPub);
    lattice::mesh::crypto::generateKeypair(r1Priv, r1Pub);
    lattice::mesh::crypto::generateKeypair(r2Priv, r2Pub);
    memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
    memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);

    PeerInfo origin{};
    memcpy(origin.mac, originMac, 6);
    memcpy(origin.publicKey, originPub, 32);
    mesh.peers.append(origin);
    PeerInfo relay1{};
    memcpy(relay1.mac, r1, 6);
    memcpy(relay1.publicKey, r1Pub, 32);
    mesh.peers.append(relay1);
    PeerInfo relay2{};
    memcpy(relay2.mac, r2, 6);
    memcpy(relay2.publicKey, r2Pub, 32);
    mesh.peers.append(relay2);

    uint8_t kUpOrigin[32], kDownOrigin[32];
    lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUpOrigin, kDownOrigin);
    uint8_t kUpR1[32], kDownR1[32];
    lattice::mesh::crypto::deriveE2EKeys(r1Priv, masterPub, kUpR1, kDownR1);
    uint8_t kUpR2[32], kDownR2[32];
    lattice::mesh::crypto::deriveE2EKeys(r2Priv, masterPub, kUpR2, kDownR2);

    mesh_message msg{};
    msg.proto_version = PROTO_VERSION;
    msg.message_type = MESH_TYPE_ROUTE_REPORT;
    memcpy(msg.origin_mac_address, originMac, 6);
    msg.route_len = 2;
    memcpy(&msg.route_path[0], r1, 6);
    memcpy(&msg.route_path[6], r2, 6);
    msg.data[0] = OP_ROUTE_REPORT;
    msg.data[1] = 2;

    uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
    uint8_t prev_hop[6] = {0};
    uint8_t ctx[routemac::HOP_CTX_LEN];
    routemac::buildHopContext(msg, prev_hop, originMac, ctx);
    routemac::chainStep(kUpOrigin, ctx, computed, computed);
    memcpy(prev_hop, originMac, 6);
    routemac::buildHopContext(msg, prev_hop, r1, ctx);
    routemac::chainStep(kUpR1, ctx, computed, computed);
    memcpy(prev_hop, r1, 6);
    routemac::buildHopContext(msg, prev_hop, r2, ctx);
    routemac::chainStep(kUpR2, ctx, computed, computed);
    memcpy(msg.auth_path, computed, routemac::AUTH_PATH_LEN);

    EXPECT_TRUE(lattice::mesh::crypto::sealPayload(kUpOrigin, msg));
    return msg;
  }
};

TEST_F(RouteReportTest, SendRouteReport_MessageType) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();

  EXPECT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
}

TEST_F(RouteReportTest, SendRouteReport_PayloadStructure) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh.sendRouteReport();

  // The wire payload is now E2E-sealed (Task 6) — open it with the same k_up
  // the master would derive before asserting on its plaintext structure.
  mesh_message sent = lastSentMsg();
  uint8_t kUp[32];
  deriveUpKey(kUp);
  ASSERT_TRUE(lattice::mesh::crypto::openPayload(kUp, sent))
      << "sent frame must open with the k_up derived the same way the master would";

  EXPECT_EQ(sent.data[0], OP_ROUTE_REPORT);  // 0xB3
  EXPECT_EQ(sent.data[1], 0);                // path_len = 0 on origin
  // data[2..63] should be zeroed
  for (int i = 2; i < 64; ++i) {
    EXPECT_EQ(sent.data[i], 0) << "data[" << i << "] should be 0";
  }
}

TEST_F(RouteReportTest, SendRouteReport_NotSentByMaster) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);
  mesh.isMaster = true;

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();  // master should be a no-op

  EXPECT_EQ(espNowSentPackets.size(), before);
}

// Task 6 (spec §4): the payload is E2E-sealed end-to-end (origin -> master),
// so an intermediate relay can no longer read or mutate it — path
// accumulation via msg.data is removed (it moves to the header route_path
// field in a future phase). A relay now forwards the sealed frame
// byte-for-byte, only updating routing metadata (hop_count/last_hop).
// Replaces the old ProcessRouteReport_RelayAppendsMAC, which asserted the
// pre-AEAD path-accumulation behavior this task intentionally removes.
TEST_F(RouteReportTest, ProcessRouteReport_RelayForwardsSealedFrameUnmodified) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t myMac[6]     = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);
  memcpy(mesh.deviceMacAddress, myMac, 6); // set after setupRelayNode (which overwrites it)

  // Foreign origin (not this relay) standing in for an already-sealed frame —
  // a relay never re-seals, only forwards. The bytes are opaque on purpose:
  // a relay must not need to interpret them.
  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = 0;
  msg.hop_count = 1;
  memcpy(msg.origin_mac_address, originMac, 6);
  for (int i = 0; i < 64; ++i)
    msg.data[i] = static_cast<uint8_t>(0xC0 + i);

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  ASSERT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
  EXPECT_EQ(memcmp(sent.data, msg.data, sizeof(sent.data)), 0)
      << "relay must forward the sealed payload byte-for-byte, unmodified (spec §4)";
  EXPECT_EQ(memcmp(sent.last_hop_mac_address, myMac, 6), 0);
  EXPECT_EQ(sent.hop_count, 2);
}

// Task 3 (Phase 3, spec §4): route_len/route_path are plaintext header fields
// excluded from the AEAD AAD (E2ECrypto.h buildAad — 24 bytes: version, type,
// data_type, origin, target, epoch, seq), so a relay can accumulate the
// origin->master relay chain there without touching (or invalidating) the
// E2E-sealed payload. This is the replacement for the msg.data-based path
// accumulation that Task 6's AEAD work removed (see
// ProcessRouteReport_RelayForwardsSealedFrameUnmodified above).
TEST_F(RouteReportTest, RelayAppendsOwnMacToRoutePath) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t myMac[6]     = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);
  memcpy(mesh.deviceMacAddress, myMac, 6); // set after setupRelayNode (which overwrites it)

  // Seal a real E2E payload (origin -> master) so this test also proves the
  // route_path/route_len write does not invalidate the AEAD tag: buildAad()
  // (E2ECrypto.h) covers only proto_version/message_type/data_type/origin/
  // target/epoch/seq — route_len and route_path are outside the AAD.
  uint8_t originPriv[32], originPub[32];
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = 0;
  msg.hop_count = 0;
  msg.route_len = 0; // origin sent an empty path
  msg.epoch_num = 5;
  msg.seq_num = 1;
  memcpy(msg.origin_mac_address, originMac, 6);
  memcpy(msg.target_mac_address, masterMac, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 0;
  uint8_t plaintext[64];
  memcpy(plaintext, msg.data, sizeof(plaintext));
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  ASSERT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  ASSERT_EQ(sent.route_len, 1) << "relay appended its own MAC";
  EXPECT_EQ(memcmp(&sent.route_path[0], myMac, 6), 0);

  // Self-review (spec §4 AAD claim): the forwarded frame's sealed payload must
  // still open with the same k_up and recover the original plaintext, proving
  // the route_path/route_len write did not break the AEAD tag.
  ASSERT_TRUE(lattice::mesh::crypto::openPayload(kUp, sent))
      << "sealed payload must still open after relay writes route_path/route_len";
  EXPECT_EQ(memcmp(sent.data, plaintext, sizeof(plaintext)), 0);
}

TEST_F(RouteReportTest, ProcessRouteReport_MasterDeliversToCallback) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  Mesh mesh;
  mesh.isMaster = true;

  // Task 6 (E2E AEAD): master opens the sealed payload before delivering to
  // the callback — register the origin as a keyed peer and seal accordingly.
  uint8_t originPriv[32], originPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo origin{};
  memcpy(origin.mac, originMac, 6);
  memcpy(origin.publicKey, originPub, 32);
  origin.lastSeenMs = 0;
  mesh.peers.append(origin);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);

  bool callbackFired = false;
  mesh_message received{};
  mesh.linkDataRecvCallback([&](const mesh_message& m) {
    callbackFired = true;
    received = m;
  });

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  memcpy(msg.origin_mac_address, originMac, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 1;

  // Chain-MAC (Phase C, spec §4 / issue #44): route_len == 0 (no relays), so
  // the only hop in the chain is the origin itself (prev_hop zeroed) —
  // mirrors what Mesh::transmitCore's origin seed would compute.
  {
    uint8_t ctx[routemac::HOP_CTX_LEN];
    uint8_t prev_hop[6] = {0};
    uint8_t zero_prev_mac[routemac::AUTH_PATH_LEN] = {0};
    routemac::buildHopContext(msg, prev_hop, originMac, ctx);
    routemac::chainStep(kUp, ctx, zero_prev_mac, msg.auth_path);
  }

  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  mesh.processRouteReport(msg);

  EXPECT_TRUE(callbackFired);
  EXPECT_EQ(received.data[0], OP_ROUTE_REPORT);
}

// Task 4 (spec §4): the master must learn the origin's relay path from the
// route report so it can later source-route a downlink back to that node.
// The path is recorded from the RAW msg's plaintext route_path/route_len
// header fields (accumulated hop-by-hop by relays), NOT from the sealed
// payload — mirrors ProcessRouteReport_MasterDeliversToCallback above for
// the E2E open/keying setup.
TEST_F(RouteReportTest, ProcessRouteReport_MasterRecordsRouteFromReport) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t r1[6]        = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  const uint8_t r2[6]        = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  Mesh mesh;
  mesh.isMaster = true;
  // isMaster is set directly above (not via setIsMaster()+init()), so the
  // RouteTable allocation Mesh::init() would normally trigger never runs —
  // do it explicitly (issue #51).
  mesh.reevaluateRouteTable();

  // Task 6 (E2E AEAD): master opens the sealed payload before delivering to
  // the callback — register the origin as a keyed peer and seal accordingly.
  // Chain-MAC (Phase C, spec §4 / issue #44): the master also needs r1/r2
  // registered as keyed peers (their own pairwise k_up with the master) so
  // it can reconstruct their hops in the chain — mirrors what real relays
  // enrolled with the master would have.
  uint8_t originPriv[32], originPub[32];
  uint8_t r1Priv[32], r1Pub[32];
  uint8_t r2Priv[32], r2Pub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  lattice::mesh::crypto::generateKeypair(r1Priv, r1Pub);
  lattice::mesh::crypto::generateKeypair(r2Priv, r2Pub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo origin{};
  memcpy(origin.mac, originMac, 6);
  memcpy(origin.publicKey, originPub, 32);
  origin.lastSeenMs = 0;
  mesh.peers.append(origin);
  PeerInfo relay1{};
  memcpy(relay1.mac, r1, 6);
  memcpy(relay1.publicKey, r1Pub, 32);
  relay1.lastSeenMs = 0;
  mesh.peers.append(relay1);
  PeerInfo relay2{};
  memcpy(relay2.mac, r2, 6);
  memcpy(relay2.publicKey, r2Pub, 32);
  relay2.lastSeenMs = 0;
  mesh.peers.append(relay2);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);
  uint8_t kUpR1[32], kDownR1[32];
  lattice::mesh::crypto::deriveE2EKeys(r1Priv, masterPub, kUpR1, kDownR1);
  uint8_t kUpR2[32], kDownR2[32];
  lattice::mesh::crypto::deriveE2EKeys(r2Priv, masterPub, kUpR2, kDownR2);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  memcpy(msg.origin_mac_address, originMac, 6);
  msg.route_len = 2;
  memcpy(&msg.route_path[0], r1, 6);
  memcpy(&msg.route_path[6], r2, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 2;

  // Chain-MAC (Phase C): hop0 = origin, hop1 = r1, hop2 = r2 — mirrors the
  // real origin-seed + relay-extend accumulation path.
  {
    uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
    uint8_t prev_hop[6] = {0};
    uint8_t ctx[routemac::HOP_CTX_LEN];
    routemac::buildHopContext(msg, prev_hop, originMac, ctx);
    routemac::chainStep(kUp, ctx, computed, computed);
    memcpy(prev_hop, originMac, 6);
    routemac::buildHopContext(msg, prev_hop, r1, ctx);
    routemac::chainStep(kUpR1, ctx, computed, computed);
    memcpy(prev_hop, r1, 6);
    routemac::buildHopContext(msg, prev_hop, r2, ctx);
    routemac::chainStep(kUpR2, ctx, computed, computed);
    memcpy(msg.auth_path, computed, routemac::AUTH_PATH_LEN);
  }

  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  mesh.processRouteReport(msg);

  uint8_t out[60];
  uint8_t len = 0;
  ASSERT_NE(mesh.testRoutes(), nullptr);
  ASSERT_TRUE(mesh.testRoutes()->lookup(originMac, out, &len));
  EXPECT_EQ(len, 2);
  EXPECT_EQ(memcmp(&out[0], r1, 6), 0);
  EXPECT_EQ(memcmp(&out[6], r2, 6), 0);
}

// Replaces the old ProcessRouteReport_PathFullDropsMessage: the path-length
// guard was part of the relay-side path-accumulation feature this task
// removes (spec §4 — a relay can no longer read msg.data at all). The
// remaining relay-side guard is the pre-existing hop-count limit.
TEST_F(RouteReportTest, ProcessRouteReport_HopLimitDropsMessage) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.hop_count = lattice::config::MAX_HOPS; // at limit — relay must not forward further
  memcpy(msg.origin_mac_address, originMac, 6);
  for (int i = 0; i < 64; ++i) // opaque payload — relay never reads it
    msg.data[i] = 0xAB;

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no message sent — hop limit reached
}

// Replaces the old ProcessRouteReport_MalformedOpcodeDropsMessage: opcode
// validation is no longer possible at a relay (payload is ciphertext to it).
// It now happens only at the master, after a successful AEAD open.
TEST_F(RouteReportTest, ProcessRouteReport_MasterDropsOnBadOpcodeAfterOpen) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  Mesh mesh;
  mesh.isMaster = true;

  uint8_t originPriv[32], originPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo origin{};
  memcpy(origin.mac, originMac, 6);
  memcpy(origin.publicKey, originPub, 32);
  origin.lastSeenMs = 0;
  mesh.peers.append(origin);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](const mesh_message&) { callbackFired = true; });

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  memcpy(msg.origin_mac_address, originMac, 6);
  msg.data[0] = 0xFF; // wrong opcode (plaintext) — sealed correctly, auth succeeds
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  mesh.processRouteReport(msg);

  EXPECT_FALSE(callbackFired)
      << "master must drop a validly-sealed frame carrying an unexpected opcode";
}

TEST_F(RouteReportTest, DrainRecvQueue_DispatchesRouteReport) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 0;

  // Directly push to recv queue (UNIT_TEST exposes all members)
  uint8_t srcMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  MeshTransport::RecvQueueEntry entry;
  memcpy(entry.srcMac, srcMac, 6);
  entry.msg = msg;
  xRingbufferSend(mesh.transport.recvQueue, &entry, sizeof(entry), 0);

  size_t before = espNowSentPackets.size();
  mesh.drain();

  EXPECT_GT(espNowSentPackets.size(), before); // relayed the message
}

// ---- Chain-MAC verify (Phase C, spec §4 / issue #44) ----

TEST_F(RouteReportTest, MasterVerifiesValidChain_RecordsPath) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t r1[6]        = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  const uint8_t r2[6]        = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  Mesh mesh;
  mesh_message msg = buildValidTwoHopChain(mesh, originMac, r1, r2);

  mesh.processRouteReport(msg);

  uint8_t out[60];
  uint8_t len = 0;
  ASSERT_NE(mesh.testRoutes(), nullptr);
  ASSERT_TRUE(mesh.testRoutes()->lookup(originMac, out, &len))
      << "a validly chain-MAC'd route report must be recorded";
  EXPECT_EQ(len, 2);
  EXPECT_EQ(memcmp(&out[0], r1, 6), 0);
  EXPECT_EQ(memcmp(&out[6], r2, 6), 0);
}

TEST_F(RouteReportTest, MasterRejectsTamperedRoutePath_NoRecord) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t r1[6]        = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  const uint8_t r2[6]        = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  Mesh mesh;
  mesh_message msg = buildValidTwoHopChain(mesh, originMac, r1, r2);

  msg.route_path[0] ^= 0x01; // flip one byte in route_path AFTER MAC-ing

  mesh.processRouteReport(msg);

  uint8_t out[60];
  uint8_t len = 0;
  ASSERT_NE(mesh.testRoutes(), nullptr);
  EXPECT_FALSE(mesh.testRoutes()->lookup(originMac, out, &len))
      << "a route report with a tampered route_path must fail chain-MAC verify and not be recorded";
}

TEST_F(RouteReportTest, MasterRejectsTamperedAuthPath_NoRecord) {
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t r1[6]        = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  const uint8_t r2[6]        = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  Mesh mesh;
  mesh_message msg = buildValidTwoHopChain(mesh, originMac, r1, r2);

  msg.auth_path[0] ^= 0x01; // flip one byte in auth_path

  mesh.processRouteReport(msg);

  uint8_t out[60];
  uint8_t len = 0;
  ASSERT_NE(mesh.testRoutes(), nullptr);
  EXPECT_FALSE(mesh.testRoutes()->lookup(originMac, out, &len))
      << "a route report with a tampered auth_path must fail chain-MAC verify and not be recorded";
}

TEST_F(RouteReportTest, MasterRejectsUnknownHop_NoRecord) {
  const uint8_t originMac[6]    = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t r1[6]           = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  const uint8_t r2[6]           = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  const uint8_t strangerMac[6]  = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x03};
  Mesh mesh;
  mesh_message msg = buildValidTwoHopChain(mesh, originMac, r1, r2);

  // Swap the second hop for a MAC that has no E2EKeyStore/peer entry on the
  // master at all — an unenrolled/unknown hop.
  memcpy(&msg.route_path[6], strangerMac, 6);

  mesh.processRouteReport(msg);

  uint8_t out[60];
  uint8_t len = 0;
  ASSERT_NE(mesh.testRoutes(), nullptr);
  EXPECT_FALSE(mesh.testRoutes()->lookup(originMac, out, &len))
      << "a route report naming an unknown hop must be dropped and not recorded";
}
