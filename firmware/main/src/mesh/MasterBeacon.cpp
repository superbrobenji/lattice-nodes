#include "MasterBeacon.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/network/MacEq.h"
#include "config/master_pubkey_pin_wrapper.h"
#include <esp_timer.h>
// Phase C finding 17: this TU calls esp_random() and, now that Logger.h no
// longer transitively pulls in ESP-IDF headers via Arduino.h, that dependency
// needs to be declared directly rather than relying on it arriving through
// some other header's include chain — esp_random() is a plain esp_hw_support
// API, not an Arduino one, so this needs no Arduino component at all.
#include <esp_random.h>
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;

bool MasterBeacon::intervalElapsed() {
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  if (now - lastBeaconMillis < lattice::config::MASTER_BEACON_INTERVAL_MS)
    return false;
  lastBeaconMillis = now;
  return true;
}

void MasterBeacon::send(const mesh_message& msg, MeshTransport& transport) {
  // Broadcast-only: send to the registered FF:FF:… broadcast peer so the frame
  // reaches all nodes — including those not yet individually registered.
  // esp_now_send(nullptr, …) only delivers to already-registered unicast peers.
  (void)transport.sendBroadcast(msg); // sendBroadcast already logs on failure
}

void MasterBeacon::checkTimeout(bool isMaster, MasterInfo& currentMaster,
                                uint8_t* lastSeenMasterMac) {
  if (isMaster)
    return;
  if (currentMaster.distance == 0xFF)
    return; // No master known yet
  if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - lastMasterBeaconReceivedMs >
      STALE_MASTER_THRESHOLD_MS) {
    LATTICE_LOGLN("MESH", "Master beacon timeout — clearing route, treating as offline",
                  LogLevel::LOG_WARN);
    memset(currentMaster.mac, 0, 6);
    currentMaster.distance = 0xFF;
    memset(lastSeenMasterMac, 0, 6);
    lastMasterBeaconReceivedMs = 0;
  }
}

void MasterBeacon::process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                           bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
                           MasterInfo& currentMaster, OutboundSequenceState& txState,
                           mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut,
                           bool& relayPendingOut, uint8_t* lastSeenMasterMac) {
  // Guard: ignore echoes of our own beacon relayed back by neighbours (relays are
  // broadcast, so the originating master hears them too). Without this the master
  // would TOFU-learn itself as knownMasterMac and record a bogus route to itself.
  if (lattice::mac::eq(msg.origin_mac_address, deviceMac))
    return;

  // Master MAC pin (Phase D, #42): the beacon's origin_mac_address must match
  // the deployment-provisioned master MAC pinned at build time. Weaker
  // guarantee than the JOIN_ACK pubkey pin — WiFi MACs are trivially
  // spoofable, so this only rejects naive attackers, not a MAC-spoofing RF
  // attacker. Runs BEFORE any TOFU state mutation. DEV_MODE (compile-time)
  // bypasses this in dev firmware builds; the UNIT_TEST-only runtime bypass
  // lets tests toggle it without recompiling.
  if (!lattice::config::DEV_MODE && !lattice::mesh::pin::isTestBypassed()) {
    if (memcmp(msg.origin_mac_address, lattice::mesh::pin::MASTER_MAC,
               sizeof(lattice::mesh::pin::MASTER_MAC)) != 0) {
      LATTICE_LOGLN("MESH", "Beacon origin MAC mismatch pin — drop", LogLevel::LOG_ERROR);
      return;
    }
  }

  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // --- TOFU master MAC enforcement ---
  bool fromPrimary = enrollment.hasKnownMaster() &&
                     lattice::mac::eq(msg.origin_mac_address, enrollment.knownMaster());
  bool fromSecondary = dualMasterMode && enrollment.hasKnownSecondaryMaster() &&
                       lattice::mac::eq(msg.origin_mac_address, enrollment.knownSecondaryMaster());

  if (!enrollment.hasKnownMaster()) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    enrollment.learnMasterMac(msg.origin_mac_address);
    LATTICE_LOGLN("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  } else if (!fromPrimary && !fromSecondary) {
    // Beacon from unrecognised MAC
    if (dualMasterMode && !enrollment.hasKnownSecondaryMaster()) {
      // Second master TOFU — learn and save as secondary
      enrollment.learnSecondaryMasterMac(msg.origin_mac_address);
      LATTICE_LOGLN("MESH", "Secondary master MAC learned (TOFU)", LogLevel::LOG_INFO);
      // fall through to process this beacon as valid
    } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - lastMasterBeaconReceivedMs <
               STALE_MASTER_THRESHOLD_MS) {
      // Known master(s) still fresh — reject unknown MAC
      LATTICE_LOGLN("MESH", "Beacon from unexpected MAC rejected (master still alive)",
                    LogLevel::LOG_WARN);
      return;
    } else {
      // All known masters stale — accept as new primary (hotswap)
      LATTICE_LOGLN("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
      enrollment.learnMasterMac(msg.origin_mac_address);
    }
  }

  if (!lattice::mac::eq(lastSeenMasterMac, msg.origin_mac_address) && lastSeenMasterMac[0] != 0) {
    if (dualMasterMode) {
      LATTICE_LOGLN("MESH", "Two masters active (dual master mode)", LogLevel::LOG_DEBUG);
    } else {
      LATTICE_LOGLN("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
      lattice::err::fail(
          lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 7,
          "Multiple master nodes detected! Network split or misconfiguration likely.");
    }
  }
  memcpy(lastSeenMasterMac, msg.origin_mac_address, 6);
  lastMasterBeaconReceivedMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;

  // Multi-hop routing (spec §3): the node we heard this beacon THROUGH
  // (last_hop) is a forwarding candidate. msg.hop_count is last_hop's OWN
  // distance to the master (this receiving node's distance is one more, per
  // `newDistance` above — last_hop is one hop closer), so last_hop's distance
  // is msg.hop_count, not +1: a direct beacon straight from the master
  // (hop_count == 0, last_hop == master) must record the master itself as a
  // distance-0 neighbor. Learned here, not from enrollment — routing only.
  //
  // Derive currentMaster.distance from live NeighborTable state (issue #45) in
  // the SAME pass as the observe (post-Phase-G audit item X) — this used to be
  // neighbors.observe(...) followed by a separate neighbors.minFreshDistance(...)
  // call, two full linear scans of the neighbor table per beacon RX.
  // Sticky-min replaced by a pure function of neighbor state: rises monotonically
  // as shorter-path neighbors age out; no oscillation because state can only
  // flap if NeighborTable itself flaps.
  memcpy(currentMaster.mac, msg.origin_mac_address, 6);
  uint8_t min_d =
      neighbors.observeAndMinDistance(msg.last_hop_mac_address, msg.hop_count,
                                      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL);
  uint8_t derived = (min_d == 0xFF) ? 0xFF : static_cast<uint8_t>(min_d + 1);
  if (derived != currentMaster.distance) {
    currentMaster.distance = derived;
    LATTICE_LOGF("MESH", LogLevel::LOG_INFO, "Route distance derived: %u", (unsigned)derived);
  }

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    if (txState.wasRelayedBefore(msg.epoch_num, msg.seq_num)) {
      LATTICE_LOGLN("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    txState.markRelayed(msg.epoch_num, msg.seq_num);

    // Defer relay with random jitter to stagger transmissions across all non-master
    // nodes and eliminate the collision burst that occurs when all nodes relay
    // within milliseconds of receiving the same beacon.
    // Jitter window: 10–73 ms (10 + esp_random() % RELAY_JITTER_MAX_MS)
    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % lattice::config::RELAY_JITTER_MAX_MS);
    relayPendingMsgOut = msg;
    // Relay carries this node's just-derived distance (`derived`, above), not
    // the naive msg.hop_count + 1 for this specific beacon's path — if a
    // shorter fresh neighbor already exists, downstream nodes should hear
    // this node's true (possibly smaller) distance, not an inflated one.
    relayPendingMsgOut.hop_count = derived;
    memcpy(relayPendingMsgOut.last_hop_mac_address, deviceMac, 6);
    relayPendingAtOut = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL + 10 + jitterMs;
    relayPendingOut = true;
  }
}

} // namespace mesh
} // namespace lattice
