#include "PeerEnrollment.h"
#include <cstring>
#include <esp_timer.h>
#include "MeshTransport.h"
#include "src/logging/Logger.h"
#include "src/network/MacEq.h"
#include "src/network/mem.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

// Moved verbatim from Mesh::registerPeerWithKey (Mesh.cpp:756-794 pre-Task-10)
// — only change: `peers`/`enrollment`/`dualMasterMode` are now explicit
// parameters instead of implicit Mesh member access. The
// enrollment.enrollPeer(...) call already routes through Enrollment's own
// ESP-NOW registration internally (Round 1's MeshTransport::registerPeerWithEspNow
// static method), so it stays unchanged here.
bool registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey,
                         PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode) {
  PeerInfo* p = peers.find(mac);
  if (p) {
    if (!allowRekey) {
      // Established (non-zero) key material must never be replaced from this
      // path — an over-the-air JOIN_ACK with a spoofed trusted origin would
      // otherwise re-key the link to attacker-chosen material. An all-zero
      // stored key is a pre-enrollment placeholder (e.g. DEFAULT_PEERS), not
      // an established key, so upgrading it is allowed.
      bool keyEstablished = !lattice::mem::is_zero(p->publicKey, 32);
      if (keyEstablished) {
        LATTICE_LOGLN("MESH", "Peer already registered — keeping established key",
                      LogLevel::LOG_DEBUG);
        p->lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        return true; // already routable; nothing to change
      }
    }
    // Update existing peer's public key
    memcpy(p->publicKey, publicKey32, 32);
    p->lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  } else {
    if (peers.count() >= MAX_PEERS) {
      LATTICE_LOGLN("MESH", "Peer list full, cannot enroll", LogLevel::LOG_WARN);
      return false;
    }
    PeerInfo newPeer;
    memcpy(newPeer.mac, mac, 6);
    memcpy(newPeer.publicKey, publicKey32, 32);
    newPeer.lastSeenMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    peers.append(newPeer);
  }
  peers.saveToEEPROM();

  // Re-register with encryption now that we have the public key (mbedtls-heavy via Enrollment)
  enrollment.enrollPeer(mac, publicKey32, nullptr, dualMasterMode);
  return true;
}

// Moved verbatim from Mesh::addPeer (Mesh.cpp:748-755 pre-Task-10).
void addPeer(const uint8_t* mac, PeerRegistry& peers) {
  size_t before = peers.count();
  peers.addAndPersist(mac);
  if (peers.count() > before) {
    MeshTransport::registerPeerWithEspNow(peers.at(peers.count() - 1).mac);
  }
}

// Moved verbatim from the body of Mesh::processJoinAck (Mesh.cpp:714-743
// pre-Task-10) — only change: transport.sendBroadcast(relay) ->
// MeshTransport::sendBroadcast(relay) (static, per Round 1 Task 4), and
// deviceMacAddress/isMaster/enrollment become explicit parameters.
void dispatchJoinAck(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                     Enrollment& enrollment, RegisterPeerFn registerFn) {
  // Relay outward if not addressed to us (multi-hop enrollment, Task 9b Bug #5
  // downlink counterpart). The target node is still mid-enrollment and is NOT
  // yet a registered unicast peer of ours, so — exactly as the master does when
  // it first emits the ACK (see enrollPeer: broadcast via the FF:FF peer) — we
  // RE-BROADCAST rather than unicast to known peers via router.relayDownlink(). Loop
  // safety: never re-broadcast a JOIN_ACK we originated (only masters originate
  // them, so this stops the master looping on its own echo), and bound depth by
  // MAX_HOPS as a backstop for cyclic topologies.
  if (!lattice::mac::eq(msg.target_mac_address, deviceMac)) {
    if (lattice::mac::eq(msg.origin_mac_address, deviceMac))
      return;
    if (msg.hop_count >= lattice::config::MAX_HOPS)
      return;
    mesh_message relay = msg;
    relay.hop_count++;
    memcpy(relay.last_hop_mac_address, deviceMac, 6);
    MeshTransport::sendBroadcast(relay);
    return;
  }
  // Masters issue JOIN_ACKs; they never enroll via one. Without this guard a
  // forged ACK addressed to the master (fingerprint is observable over the
  // air) could TOFU-poison it and register attacker key material.
  if (isMaster) {
    LATTICE_LOGLN("MESH", "JOIN_ACK addressed to master — ignoring", LogLevel::LOG_WARN);
    return;
  }
  enrollment.processJoinAck(msg, deviceMac, registerFn);
}

} // namespace mesh
} // namespace lattice
