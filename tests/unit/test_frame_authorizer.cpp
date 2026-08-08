// Direct unit tests for FrameAuthorizer::authorize (round 2 task 13) — the
// plan's most security-sensitive extraction. These exercise the authorization
// decision in isolation, independent of processAdapterData's routing/relay
// wrapper (see AdapterDataRelayTest / ConfigOpcodeInjectionTest in
// test_mesh_logic.cpp for the integration-level coverage of the same logic
// via the full Mesh::processAdapterData path). Per the design spec's Task 13
// note (and the lesson from Round 1 Task 6's DropHopLimitExceeded fix-round,
// which shipped with zero direct tests until review caught it), this
// authorization gate gets direct tests proactively rather than relying on
// integration coverage alone.
#include <gtest/gtest.h>
#include <cstring>
#include "mesh/FrameAuthorizer.h"
#include "mesh/E2EKeyLookup.h"
#include "mesh/E2ECrypto.h"
#include "mesh/MeshCrypto.h"
#include "mesh/MeshMessenger.h" // PROTO_VERSION, isSealedType (single definitions, fix round 1)
#include "src/adapter/Adapter.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "EEPROM.h"
#include "time_mock.h"

using namespace lattice::mesh;
using lattice::adapter::adapter_types;

class FrameAuthorizerTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kOtherMasterMac[6] = {0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
  static constexpr uint8_t kSensorMac[6] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
  static constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  FrameAuthorizer authorizer;
  PeerRegistry peers;
  Enrollment enrollment;
  E2EKeyStore e2eKeys;
  MasterInfo currentMaster{};
};

constexpr uint8_t FrameAuthorizerTest::kMyMac[];
constexpr uint8_t FrameAuthorizerTest::kMasterMac[];
constexpr uint8_t FrameAuthorizerTest::kOtherMasterMac[];
constexpr uint8_t FrameAuthorizerTest::kSensorMac[];
constexpr uint8_t FrameAuthorizerTest::kBroadcastMac[];

// ─── Rejection paths ──────────────────────────────────────────────────────

TEST_F(FrameAuthorizerTest, RejectsSealedTypeNotAddressedToSelfAtMaster) {
  // Master-not-self-addressed sealed-type gate: a sealed ADAPTER_DATA frame
  // arriving at the master with a broadcast (not-self) target is either a
  // stale self-echo or a forgery — must be rejected before any E2E open is
  // even attempted (no peer/key setup needed for this test to pass).
  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA; // sealed type
  memcpy(msg.origin_mac_address, kSensorMac, 6);
  memcpy(msg.target_mac_address, kBroadcastMac, 6); // NOT addressed to self

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/true, /*addressedToSelf=*/false,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  EXPECT_EQ(result, AuthResult::Rejected);
}

TEST_F(FrameAuthorizerTest, RejectsWhenMasterSideE2EOpenFails) {
  // isMaster=true, addressedToSelf=true, ADAPTER_DATA (sealed), but the
  // origin is an unknown/unenrolled peer — peers.find() returns nullptr, so
  // lattice::mesh::peerE2EKeys() fails before openPayload ever runs.
  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kSensorMac, 6); // never registered in `peers`
  memcpy(msg.target_mac_address, kMasterMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 1;

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/true, /*addressedToSelf=*/true,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  EXPECT_EQ(result, AuthResult::Rejected);
}

TEST_F(FrameAuthorizerTest, RejectsWhenNodeSideE2EOpenFails) {
  // isMaster=false, addressedToSelf=true, ADAPTER_DATA, but this node is not
  // enrolled (enrollment.hasKnownMaster() == false, the default) —
  // lattice::mesh::masterE2EKeys() fails immediately.
  memcpy(currentMaster.mac, kMasterMac, 6);
  currentMaster.distance = 1;
  // enrollment left default-constructed: hasMasterMac == false.

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  memcpy(msg.origin_mac_address, kMasterMac, 6);
  memcpy(msg.target_mac_address, kMyMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 1;

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/false, /*addressedToSelf=*/true,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  EXPECT_EQ(result, AuthResult::Rejected);
}

TEST_F(FrameAuthorizerTest, RejectsConfigOpcodeViaUnopenedBroadcastPath) {
  // The specific forged-broadcast attack Mesh.cpp's original comments
  // document: BROADCAST target, SERIAL_ADAPTER data_type, data[0] ==
  // OP_CONFIG_SET, origin spoofed to the master's public MAC (observable over
  // the air in beacons), NOT sealed — exactly what an RF attacker can send
  // with zero key material. Never addressedToSelf (broadcast target), so it
  // is never opened. Reconstructed faithfully even though this node IS
  // legitimately enrolled with kMasterMac (real keys, real peer entry) — the
  // attack must be rejected regardless of the victim's enrollment state,
  // since the forged frame never goes through the E2E-open path at all.
  memcpy(currentMaster.mac, kMasterMac, 6);
  currentMaster.distance = 1;
  enrollment.hasMasterMac = true;
  memcpy(enrollment.knownMasterMac, kMasterMac, 6);
  uint8_t nodePriv[32], nodePub[32], masterPriv[32], masterPub[32];
  lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  memcpy(enrollment.devicePrivateKey, nodePriv, 32);
  memcpy(enrollment.devicePublicKey, nodePub, 32);
  PeerInfo master{};
  memcpy(master.mac, kMasterMac, 6);
  memcpy(master.publicKey, masterPub, 32);
  peers.append(master);

  mesh_message forged{};
  forged.proto_version = PROTO_VERSION;
  forged.message_type = MESH_TYPE_ADAPTER_DATA;
  forged.data_type = adapter_types::SERIAL_ADAPTER;
  memcpy(forged.origin_mac_address, kMasterMac, 6);    // spoofed: master's public MAC
  memcpy(forged.target_mac_address, kBroadcastMac, 6); // forged: broadcast, never opened
  memcpy(forged.last_hop_mac_address, kMasterMac, 6);
  forged.epoch_num = 1;
  forged.seq_num = 1;
  forged.data[0] = OP_CONFIG_SET;
  memcpy(&forged.data[1], kMyMac, 6); // victim's own MAC as the opcode's target field
  forged.data[7] = 0x02;              // attacker-chosen adapter type
  // NOT sealed — plaintext, exactly as an RF attacker would send it.

  mesh_message opened{};
  AuthResult result = authorizer.authorize(forged, /*isMaster=*/false, /*addressedToSelf=*/false,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  EXPECT_EQ(result, AuthResult::Rejected)
      << "a forged plaintext BROADCAST CONFIG_SET must never authorize — it was never "
         "addressedToSelf, so it was never opened/authenticated with k_down";
}

TEST_F(FrameAuthorizerTest, RejectsConfigOpcodeFromNonMasterOrigin) {
  // Sealed and opened successfully via the real currentMaster relationship
  // (registered peer + matching keys — the E2E open genuinely succeeds), but
  // enrollment's TOFU-learned master identity (knownMasterMac) is a
  // DIFFERENT MAC than the one that sealed this frame, and no secondary
  // master is configured either. This deliberately decouples currentMaster
  // (what masterE2EKeys() derives k_down from) from knownMasterMac (what the
  // config-opcode origin gate checks against) — in production the two are
  // kept in sync by MasterBeacon's TOFU learning, but FrameAuthorizer is
  // tested here in isolation from that invariant, exactly so this gate's own
  // behavior is pinned independently of it.
  memcpy(currentMaster.mac, kMasterMac, 6);
  currentMaster.distance = 1;
  uint8_t nodePriv[32], nodePub[32], masterPriv[32], masterPub[32];
  lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  memcpy(enrollment.devicePrivateKey, nodePriv, 32);
  memcpy(enrollment.devicePublicKey, nodePub, 32);
  PeerInfo master{};
  memcpy(master.mac, kMasterMac, 6);
  memcpy(master.publicKey, masterPub, 32);
  peers.append(master);
  enrollment.hasMasterMac = true;
  memcpy(enrollment.knownMasterMac, kOtherMasterMac, 6); // does NOT match kMasterMac
  enrollment.hasMasterMacSecondary = false;

  const uint8_t *kUp, *kDown;
  ASSERT_TRUE(
      lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown));

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::SERIAL_ADAPTER;
  memcpy(msg.origin_mac_address, kMasterMac, 6); // matches the sealing peer, NOT knownMasterMac
  memcpy(msg.target_mac_address, kMyMac, 6);
  memcpy(msg.last_hop_mac_address, kMasterMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 1;
  msg.data[0] = OP_CONFIG_SET;
  memcpy(&msg.data[1], kMyMac, 6);
  msg.data[7] = 0x02;
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kDown, msg));

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/false, /*addressedToSelf=*/true,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  EXPECT_EQ(result, AuthResult::Rejected);
}

// ─── Legitimate paths ─────────────────────────────────────────────────────

TEST_F(FrameAuthorizerTest, AuthorizesLegitimateMasterSideUplink) {
  // isMaster=true, addressedToSelf=true, valid E2E keys (peerE2EKeys), a
  // non-config opcode. openedOut must contain the decrypted payload.
  uint8_t masterPriv[32], masterPub[32], sensorPriv[32], sensorPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(sensorPriv, sensorPub);
  memcpy(enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(enrollment.devicePublicKey, masterPub, 32);
  PeerInfo sensorPeer{};
  memcpy(sensorPeer.mac, kSensorMac, 6);
  memcpy(sensorPeer.publicKey, sensorPub, 32);
  peers.append(sensorPeer);

  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(sensorPriv, masterPub, kUp, kDown);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kSensorMac, 6);
  memcpy(msg.target_mac_address, kMasterMac, 6); // addressed to self (master)
  msg.epoch_num = 1;
  msg.seq_num = 1;
  msg.data[0] = 0x01; // arbitrary non-config payload byte
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/true, /*addressedToSelf=*/true,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  ASSERT_EQ(result, AuthResult::Authorized);
  EXPECT_EQ(opened.data[0], 0x01) << "openedOut must contain the decrypted payload";
  EXPECT_EQ(memcmp(opened.origin_mac_address, kSensorMac, 6), 0);
}

TEST_F(FrameAuthorizerTest, AuthorizesLegitimateNodeSideDownlink) {
  // isMaster=false, addressedToSelf=true, valid E2E keys (masterE2EKeys), a
  // non-config opcode. openedOut must contain the decrypted payload.
  memcpy(currentMaster.mac, kMasterMac, 6);
  currentMaster.distance = 1;
  uint8_t nodePriv[32], nodePub[32], masterPriv[32], masterPub[32];
  lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  memcpy(enrollment.devicePrivateKey, nodePriv, 32);
  memcpy(enrollment.devicePublicKey, nodePub, 32);
  enrollment.hasMasterMac = true;
  memcpy(enrollment.knownMasterMac, kMasterMac, 6);
  PeerInfo master{};
  memcpy(master.mac, kMasterMac, 6);
  memcpy(master.publicKey, masterPub, 32);
  peers.append(master);

  const uint8_t *kUp, *kDown;
  ASSERT_TRUE(
      lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown));

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kMasterMac, 6);
  memcpy(msg.target_mac_address, kMyMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 1;
  msg.data[0] = 0x02; // arbitrary non-config payload byte
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kDown, msg));

  mesh_message opened{};
  AuthResult result = authorizer.authorize(msg, /*isMaster=*/false, /*addressedToSelf=*/true,
                                           currentMaster, peers, enrollment, e2eKeys, opened);

  ASSERT_EQ(result, AuthResult::Authorized);
  EXPECT_EQ(opened.data[0], 0x02) << "openedOut must contain the decrypted payload";
}
