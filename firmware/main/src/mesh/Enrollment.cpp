#include "Enrollment.h"
#include "MeshCrypto.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "src/adapter/Adapter.h"
#include "src/network/MacEq.h"
#include "config/master_pubkey_pin_wrapper.h"
#include "project_config.h"
// Mesh.h (post-Phase-G audit item U): only for the static Mesh::sendBroadcast
// choke point below — Enrollment has no Mesh* of its own (see Enrollment.h),
// so it calls the static overload directly rather than going through an
// instance.
#include "Mesh.h"
#include <esp_now.h>
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;
using lattice::adapter::adapter_types;

Enrollment::Enrollment() {
  memset(devicePrivateKey, 0, 32);
  memset(devicePublicKey, 0, 32);
  memset(knownMasterMac, 0xFF, 6);
  memset(knownMasterMacSecondary, 0xFF, 6);
}

// NOTE: Enrollment::init() and Enrollment::enrollPeer() are mbedtls-heavy.
// Host test builds compile them for real against a host-built mbedtls (see tests/CMakeLists.txt).

void Enrollment::init() {
  auto& em = EepromManager::getInstance();
  if (em.loadKeypair(devicePrivateKey, devicePublicKey)) {
    LATTICE_LOGLN("MESH", "Device keypair loaded from EEPROM", LogLevel::LOG_INFO);
  } else {
    LATTICE_LOGLN("MESH", "Generating new Curve25519 keypair...", LogLevel::LOG_INFO);
    lattice::mesh::crypto::generateKeypair(devicePrivateKey, devicePublicKey);
    em.saveKeypair(devicePrivateKey, devicePublicKey);
    LATTICE_LOGLN("MESH", "New keypair generated and saved", LogLevel::LOG_INFO);
  }
  hasMasterMac = em.loadKnownMasterMac(knownMasterMac);
  if (hasMasterMac) {
    LATTICE_LOGLN("MESH", "Known master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
  hasMasterMacSecondary = em.loadKnownMasterMacSecondary(knownMasterMacSecondary);
  if (hasMasterMacSecondary) {
    LATTICE_LOGLN("MESH", "Known secondary master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
  _enrolled = em.loadEnrolledFlag();
}

bool Enrollment::isEnrolled() const {
  return _enrolled;
}

void Enrollment::sendRequest(const uint8_t* deviceMac, uint8_t protoVersion, uint32_t epochNum,
                             uint16_t seqNum) {
  mesh_message msg = {};
  // Stamp proto_version + (epoch, seq) so the existing ReplayCache dedups
  // relayed/reflected copies of this request (Task 9c R1): a node broadcasts the
  // SAME (origin, epoch, seq) once per retry round, so the master drops the copy
  // it also hears relayed by a neighbour. A fresh seq on each 10s retry keeps
  // legitimate re-requests from being suppressed.
  msg.proto_version = protoVersion;
  msg.epoch_num = epochNum;
  msg.seq_num = seqNum;
  msg.message_type = MESH_TYPE_ENROLLMENT;
  msg.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.origin_mac_address, deviceMac, 6);
  memset(msg.target_mac_address, 0xFF, 6);
  memcpy(msg.last_hop_mac_address, deviceMac, 6);
  msg.hop_count = 0;
  memcpy(msg.enrollment_public_key, devicePublicKey, 32);

  Mesh::sendBroadcast(msg);
  LATTICE_LOGLN("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}

void Enrollment::processRequest(const mesh_message& msg) {
  enqueuePendingRelay(msg.origin_mac_address, msg.enrollment_public_key);
  LATTICE_LOGLN("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}

void Enrollment::enqueuePendingRelay(const uint8_t* mac, const uint8_t* pubKey) {
  if (_pendingRelayCount >= PENDING_RELAY_QUEUE_SIZE) {
    LATTICE_LOGLN("MESH", "Enrollment relay queue full — dropping request", LogLevel::LOG_WARN);
    return;
  }
  size_t idx = (_pendingRelayHead + _pendingRelayCount) % PENDING_RELAY_QUEUE_SIZE;
  memcpy(_pendingRelayQueue[idx].mac, mac, 6);
  memcpy(_pendingRelayQueue[idx].pubKey, pubKey, 32);
  _pendingRelayCount++;
}

void Enrollment::processJoinAck(const mesh_message& msg, const uint8_t* /*deviceMac*/,
                                RegisterPeerFn registerFn) {
  // Called only when msg.target_mac_address == deviceMacAddress (Mesh checks this before calling)
  if (memcmp(msg.data, devicePublicKey, 4) != 0) {
    LATTICE_LOGLN("MESH", "JOIN_ACK fingerprint mismatch — ignoring", LogLevel::LOG_WARN);
    return;
  }

  // Master pubkey pin (Phase D, #42): the JOIN_ACK's enrollment_public_key must
  // match the deployment-provisioned master pubkey pinned at build time. Strong
  // authentication — an RF-present attacker cannot forge a JOIN_ACK the pin will
  // accept without the master's private key. Runs BEFORE the TOFU origin gate
  // (below) and any state mutation / peer registration. DEV_MODE (compile-time)
  // bypasses this in dev firmware builds; the UNIT_TEST-only runtime bypass lets
  // tests toggle it without recompiling.
  if (!lattice::config::DEV_MODE && !lattice::mesh::pin::isTestBypassed()) {
    if (memcmp(msg.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY,
               sizeof(lattice::mesh::pin::MASTER_PUBKEY)) != 0) {
      LATTICE_LOGLN("ENROLL", "JOIN_ACK master pubkey mismatch pin — drop", LogLevel::LOG_ERROR);
      return;
    }
  }

  // TOFU origin gate: JOIN_ACKs arrive over the unencrypted broadcast peer and
  // the fingerprint above is observable over the air (it is broadcast in our own
  // ENROLLMENT requests), so it does NOT authenticate the sender. Once a master
  // MAC is known (from enrollment or the beacon TOFU fallback), only that origin
  // may deliver a JOIN_ACK; anything else is a forgery and must not enroll us,
  // TOFU-learn, or touch peer key material.
  if (hasMasterMac && !lattice::mac::eq(msg.origin_mac_address, knownMasterMac)) {
    LATTICE_LOGLN("MESH", "JOIN_ACK from unexpected origin — ignoring", LogLevel::LOG_WARN);
    return;
  }

  // Register the approving master as a routable peer. Mesh::findNextHopToMaster()
  // can only route through PeerRegistry entries, so without this the enrolled
  // node has no uplink route (adapter data / route reports toward the master).
  // The JOIN_ACK carries the master's public key in enrollment_public_key
  // (mirroring how the master registers the node with the node's key). The
  // Mesh-provided registerFn is add-only: it never replaces an established
  // peer key, so even an origin-spoofed forgery cannot re-key a trusted link.
  // If registration fails (registry full), do NOT mark enrolled or TOFU-learn —
  // an "enrolled" node without an uplink route is worse than retrying.
  if (registerFn && !registerFn(msg.origin_mac_address, msg.enrollment_public_key)) {
    LATTICE_LOGLN("MESH", "JOIN_ACK peer registration failed — not enrolling", LogLevel::LOG_ERROR);
    return;
  }

  LATTICE_LOGLN("MESH", "Enrollment approved! Saving enrolled flag.", LogLevel::LOG_INFO);
  EepromManager::getInstance().saveEnrolledFlag(true);
  _enrolled = true;

  // The node sending JOIN_ACK is the master — record its MAC (TOFU)
  if (!hasMasterMac) {
    memcpy(knownMasterMac, msg.origin_mac_address, 6);
    hasMasterMac = true;
    EepromManager::getInstance().saveKnownMasterMac(knownMasterMac);
    LATTICE_LOGLN("MESH", "Master MAC learned and saved (TOFU)", LogLevel::LOG_INFO);
  }

  // Dual-master (spec §5, wire shrink §8): if the server included a secondary
  // master, register it as a peer (persists mac+pubkey, so masterE2EKeys can
  // derive against it after failover) and record it as the secondary for
  // beacon adoption. Protocol v0.6.0 packs the secondary into the JOIN_ACK
  // data[] payload instead of top-level MeshMessage fields:
  //   data[0..4]   = node pubkey fingerprint (already verified above)
  //   data[4..10]  = secondaryMasterMac (zero if single-master)
  //   data[10..42] = secondaryPublicKey (zero if single-master)
  //   data[42..64] = zero
  // AEAD authTag still covers data[64] exactly, so these bytes are
  // AEAD-protected same as before. Guarded on a non-zero secondary MAC.
  const uint8_t* secondaryMasterMac = msg.data + 4;
  const uint8_t* secondaryPublicKey = msg.data + 10;
  bool hasSecondary = false;
  for (int i = 0; i < 6; ++i)
    if (secondaryMasterMac[i]) {
      hasSecondary = true;
      break;
    }
  if (hasSecondary) {
    bool secondaryRegistered = registerFn && registerFn(secondaryMasterMac, secondaryPublicKey);
    if (secondaryRegistered && !hasMasterMacSecondary) {
      memcpy(knownMasterMacSecondary, secondaryMasterMac, 6);
      hasMasterMacSecondary = true;
      EepromManager::getInstance().saveKnownMasterMacSecondary(knownMasterMacSecondary);
    }
  }
}

void Enrollment::enrollPeer(const uint8_t* mac, const uint8_t* pubKey32, RegisterPeerFn registerFn,
                            bool /*dualMasterMode*/) {
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
  crypto::registerPeerWithEspNow(mac);
  if (registerFn)
    registerFn(mac, pubKey32);
}

void Enrollment::setRelayFn(EnrollmentRelayFn fn) {
  _enrollmentRelayFn = fn;
}

void Enrollment::setPendingRelay(const uint8_t* mac, const uint8_t* pubKey) {
  enqueuePendingRelay(mac, pubKey);
}

void Enrollment::drainPendingRelay() {
  // Drain EVERY queued entry per call so concurrent enrollments are not starved.
  while (_pendingRelayCount > 0) {
    const PendingRelay& e = _pendingRelayQueue[_pendingRelayHead];
    if (_enrollmentRelayFn) {
      _enrollmentRelayFn(e.mac, e.pubKey);
    }
    _pendingRelayHead = (_pendingRelayHead + 1) % PENDING_RELAY_QUEUE_SIZE;
    _pendingRelayCount--;
  }
}

} // namespace mesh
} // namespace lattice
