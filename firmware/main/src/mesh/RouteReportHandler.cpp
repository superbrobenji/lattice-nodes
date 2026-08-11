#include "RouteReportHandler.h"
#include <cstring>
#include <esp_timer.h>
#include "E2ECrypto.h"
#include "E2EKeyLookup.h"
#include "RouteMac.h"
#include "src/logging/Logger.h"
#include "../../project_config.h"
#include "lib/lattice-protocol/c/opcodes.h"

namespace lattice {
namespace mesh {

bool RouteReportHandler::sendRouteReport(bool isMaster, UplinkRouter& uplinkRouter,
                                         MasterInfo& currentMaster, PeerRegistry& peers,
                                         NeighborTable& neighbors, Enrollment& enrollment,
                                         E2EKeyStore& e2eKeys, const uint8_t* deviceMac,
                                         OutboundSequenceState& txState, MeshMessenger& messenger,
                                         MeshTransport& transport) {
  if (isMaster)
    return false;
  if (!uplinkRouter.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac,
                                        static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL))
    return false;
  uint8_t data[64] = {};
  data[0] = OP_ROUTE_REPORT;
  data[1] = 0; // path_len — reserved; relays no longer accumulate here (spec §4)
  messenger.transmitCore(adapter_types::UNKNOWN_ADAPTER, data, MESH_TYPE_ROUTE_REPORT, nullptr,
                         isMaster, deviceMac, currentMaster, txState, peers, enrollment, e2eKeys,
                         uplinkRouter, neighbors, transport);
  return true;
}

void RouteReportHandler::processRouteReport(
    const mesh_message& msg, bool isMaster, PeerRegistry& peers, Enrollment& enrollment,
    E2EKeyStore& e2eKeys, RouteTable* routes, const uint8_t* deviceMac, MasterInfo& currentMaster,
    OutboundSequenceState& txState, MeshMessenger& messenger, UplinkRouter& uplinkRouter,
    NeighborTable& neighbors, MeshTransport& transport, ExternalRecvCallback externalRecvCallback) {
  if (isMaster) {
    // E2E open (spec §2): master unseals self-targeted uplink before parsing
    // the opcode/path bytes — the payload is ciphertext until opened.
    mesh_message opened = msg;
    const uint8_t *kUp, *kDown;
    if (!lattice::mesh::peerE2EKeys(msg.origin_mac_address, peers, enrollment, e2eKeys, &kUp,
                                    &kDown) ||
        !lattice::mesh::crypto::openPayload(kUp, opened)) {
      LATTICE_LOGLN("MESH", "E2E open failed — route report dropped", LogLevel::LOG_WARN);
      return;
    }
    if (opened.data[0] != OP_ROUTE_REPORT) {
      LATTICE_LOGLN("MESH", "processRouteReport: bad opcode, dropping", LogLevel::LOG_WARN);
      return;
    }
    if (msg.route_len > lattice::config::MAX_HOPS) {
      // Tiger-Style: bounds-check before indexing route_path below — a
      // corrupt/hostile route_len must never drive an out-of-bounds read.
      LATTICE_LOGLN("MESH", "Route report: route_len exceeds MAX_HOPS, dropping",
                    LogLevel::LOG_ERROR);
      return;
    }

    // Chain-MAC verify (Phase C, spec §4 / issue #44): reconstruct the same
    // per-hop HMAC chain the origin seeded and each relay extended, keyed
    // off each hop's own pairwise k_up with this master, and compare against
    // msg.auth_path. route_path never records the origin's own MAC (only
    // relay-appended hops — see RelayAppendsOwnMacToRoutePath), so hop 0 is
    // always the origin itself; kUp (derived above for the E2E open) is
    // reused immediately here rather than re-derived, since a subsequent
    // getKeys() call below (for a different peer) can evict/invalidate it
    // (E2EKeyStore.h — "must use immediately, not cache across calls").
    uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
    uint8_t prev_hop[6] = {0};
    {
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, msg.origin_mac_address, ctx);
      routemac::chainStep(kUp, ctx, computed, computed);
      memcpy(prev_hop, msg.origin_mac_address, 6);
    }
    for (uint8_t i = 0; i < msg.route_len; ++i) {
      const uint8_t* hop_mac = &msg.route_path[static_cast<size_t>(i) * 6];
      const uint8_t *hopKUp, *hopKDown;
      if (!lattice::mesh::peerE2EKeys(hop_mac, peers, enrollment, e2eKeys, &hopKUp, &hopKDown)) {
        LATTICE_LOGLN("MESH", "Route report: unknown hop, dropping", LogLevel::LOG_ERROR);
        return;
      }
      uint8_t ctx[routemac::HOP_CTX_LEN];
      routemac::buildHopContext(msg, prev_hop, hop_mac, ctx);
      routemac::chainStep(hopKUp, ctx, computed, computed);
      memcpy(prev_hop, hop_mac, 6);
    }
    if (memcmp(computed, msg.auth_path, routemac::AUTH_PATH_LEN) != 0) {
      LATTICE_LOGLN("MESH", "Route report: MAC verify failed, dropping", LogLevel::LOG_ERROR);
      return;
    }

    // Learn the origin's relay path for downlink source routing (spec §4).
    // route_path/route_len are plaintext header fields (accumulated by relays);
    // bounds-checked by RouteTable::record. Only recorded on MAC-verify pass.
    if (routes) {
      routes->record(msg.origin_mac_address, msg.route_path, msg.route_len,
                     static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL);
    }
    // Terminal endpoint — deliver to server via external callback
    if (externalRecvCallback)
      externalRecvCallback(opened);
    return;
  }

  // Relay node (spec §4): the payload is E2E-sealed origin->master and opaque to
  // us. Accumulate the relay path in the plaintext route_path header (excluded
  // from AAD, so this does not break the tag) so the master learns the full
  // origin->master relay chain for downlink source routing.
  if (msg.hop_count >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "processRouteReport: hop limit reached, dropping", LogLevel::LOG_WARN);
    return;
  }
  if (msg.route_len >= lattice::config::MAX_HOPS) {
    LATTICE_LOGLN("MESH", "route report path full — dropping", LogLevel::LOG_WARN);
    return;
  }

  // Chain-MAC extend (Phase C, spec §4 / issue #44): snapshot the previous
  // last hop BEFORE appending this relay's own MAC to route_path. route_path
  // never records the origin's own MAC (only relay-appended hops — see
  // RelayAppendsOwnMacToRoutePath), so when this is the first relay
  // (route_len == 0) the "previous hop" is the origin itself, not a
  // route_path entry. Reads from msg (pre-copy) — identical to relay at this
  // point, but relay isn't constructed until the next line.
  uint8_t prev_hop[6];
  if (msg.route_len == 0) {
    memcpy(prev_hop, msg.origin_mac_address, 6);
  } else {
    memcpy(prev_hop, &msg.route_path[static_cast<size_t>(msg.route_len - 1) * 6], 6);
  }

  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMac, 6);
  memcpy(&relay.route_path[static_cast<size_t>(relay.route_len) * 6], deviceMac, 6);
  relay.route_len++;

  // Fold this relay's hop into msg.auth_path, keyed off its own pairwise
  // k_up with the master — the same key material the E2E seal path already
  // relies on (lattice::mesh::masterE2EKeys), so no new provisioning is
  // needed. If the master isn't known yet (rare — relay would also fail the
  // routing lookup above), drop and log rather than forwarding an
  // unauthenticated hop.
  const uint8_t *kUp, *kDown;
  if (!lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown)) {
    LATTICE_LOGLN("MESH", "Route report: no k_up for master, dropping relay hop",
                  LogLevel::LOG_WARN);
    return;
  }
  uint8_t ctx[routemac::HOP_CTX_LEN];
  routemac::buildHopContext(relay, prev_hop, deviceMac, ctx);
  routemac::chainStep(kUp, ctx, relay.auth_path, relay.auth_path);

  messenger.transmitCore(static_cast<adapter_types>(relay.data_type), relay.data,
                         MESH_TYPE_ROUTE_REPORT, &relay, isMaster, deviceMac, currentMaster,
                         txState, peers, enrollment, e2eKeys, uplinkRouter, neighbors, transport);
}

} // namespace mesh
} // namespace lattice
