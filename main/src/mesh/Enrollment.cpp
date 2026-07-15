#include "Enrollment.h"
#include "MeshCrypto.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "src/adapter/Adapter.h"
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
    Logger::logln("MESH", "Device keypair loaded from EEPROM", LogLevel::LOG_INFO);
  } else {
    Logger::logln("MESH", "Generating new Curve25519 keypair...", LogLevel::LOG_INFO);
    lattice::mesh::crypto::generateKeypair(devicePrivateKey, devicePublicKey);
    em.saveKeypair(devicePrivateKey, devicePublicKey);
    Logger::logln("MESH", "New keypair generated and saved", LogLevel::LOG_INFO);
  }
  hasMasterMac = em.loadKnownMasterMac(knownMasterMac);
  if (hasMasterMac) {
    Logger::logln("MESH", "Known master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
  hasMasterMacSecondary = em.loadKnownMasterMacSecondary(knownMasterMacSecondary);
  if (hasMasterMacSecondary) {
    Logger::logln("MESH", "Known secondary master MAC loaded from EEPROM", LogLevel::LOG_INFO);
  }
}

bool Enrollment::isEnrolled() const {
  return EepromManager::getInstance().loadEnrolledFlag();
}

void Enrollment::sendRequest(const uint8_t* deviceMac, SendMessageFn /*sendFn*/) {
  mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  msg.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.origin_mac_address, deviceMac, 6);
  memset(msg.target_mac_address, 0xFF, 6);
  memcpy(msg.last_hop_mac_address, deviceMac, 6);
  msg.hop_count = 0;
  memcpy(msg.enrollment_public_key, devicePublicKey, 32);

  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  Logger::logln("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}

void Enrollment::processRequest(const mesh_message& msg) {
  enqueuePendingRelay(msg.origin_mac_address, msg.enrollment_public_key);
  Logger::logln("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}

void Enrollment::enqueuePendingRelay(const uint8_t* mac, const uint8_t* pubKey) {
  if (_pendingRelayCount >= PENDING_RELAY_QUEUE_SIZE) {
    Logger::logln("MESH", "Enrollment relay queue full — dropping request", LogLevel::LOG_WARN);
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
    Logger::logln("MESH", "JOIN_ACK fingerprint mismatch — ignoring", LogLevel::LOG_WARN);
    return;
  }
  // TOFU origin gate: JOIN_ACKs arrive over the unencrypted broadcast peer and
  // the fingerprint above is observable over the air (it is broadcast in our own
  // ENROLLMENT requests), so it does NOT authenticate the sender. Once a master
  // MAC is known (from enrollment or the beacon TOFU fallback), only that origin
  // may deliver a JOIN_ACK; anything else is a forgery and must not enroll us,
  // TOFU-learn, or touch peer key material.
  if (hasMasterMac && memcmp(msg.origin_mac_address, knownMasterMac, 6) != 0) {
    Logger::logln("MESH", "JOIN_ACK from unexpected origin — ignoring", LogLevel::LOG_WARN);
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
    Logger::logln("MESH", "JOIN_ACK peer registration failed — not enrolling", LogLevel::LOG_ERROR);
    return;
  }

  Logger::logln("MESH", "Enrollment approved! Saving enrolled flag.", LogLevel::LOG_INFO);
  EepromManager::getInstance().saveEnrolledFlag(true);

  // The node sending JOIN_ACK is the master — record its MAC (TOFU)
  if (!hasMasterMac) {
    memcpy(knownMasterMac, msg.origin_mac_address, 6);
    hasMasterMac = true;
    EepromManager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned and saved (TOFU)", LogLevel::LOG_INFO);
  }
}

void Enrollment::enrollPeer(const uint8_t* mac, const uint8_t* pubKey32, RegisterPeerFn registerFn,
                            bool /*dualMasterMode*/) {
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
  crypto::registerPeerWithEspNow(mac, devicePrivateKey, pubKey32);
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
