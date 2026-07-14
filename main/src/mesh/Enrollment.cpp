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

Enrollment::Enrollment() {
  memset(devicePrivateKey, 0, 32);
  memset(devicePublicKey, 0, 32);
  memset(knownMasterMac, 0xFF, 6);
  memset(knownMasterMacSecondary, 0xFF, 6);
  memset(_pendingEnrollmentMac, 0, 6);
  memset(_pendingEnrollmentPubKey, 0, 32);
}

// NOTE: Enrollment::init() and Enrollment::enrollPeer() are mbedtls-heavy.
// In test builds, these are STUBBED in firmware_stubs.cpp.
// The remaining methods compile cleanly without mbedtls.

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
  memcpy(_pendingEnrollmentMac, msg.origin_mac_address, 6);
  memcpy(_pendingEnrollmentPubKey, msg.enrollment_public_key, 32);
  _pendingEnrollmentRelay = true;
  Logger::logln("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}

void Enrollment::processJoinAck(const mesh_message& msg, const uint8_t* /*deviceMac*/,
                                 RegisterPeerFn /*registerFn*/) {
  // Called only when msg.target_mac_address == deviceMacAddress (Mesh checks this before calling)
  if (memcmp(msg.data, devicePublicKey, 4) != 0) {
    Logger::logln("MESH", "JOIN_ACK fingerprint mismatch — ignoring", LogLevel::LOG_WARN);
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

void Enrollment::enrollPeer(const uint8_t mac[6], const uint8_t pubKey32[32],
                             RegisterPeerFn registerFn, bool /*dualMasterMode*/) {
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

void Enrollment::setPendingRelay(const uint8_t mac[6], const uint8_t pubKey[32]) {
  memcpy(_pendingEnrollmentMac, mac, 6);
  memcpy(_pendingEnrollmentPubKey, pubKey, 32);
  _pendingEnrollmentRelay = true;
}

void Enrollment::drainPendingRelay() {
  if (!_pendingEnrollmentRelay)
    return;
  _pendingEnrollmentRelay = false;
  if (_enrollmentRelayFn) {
    _enrollmentRelayFn(_pendingEnrollmentMac, _pendingEnrollmentPubKey);
  }
}

} // namespace mesh
} // namespace lattice
