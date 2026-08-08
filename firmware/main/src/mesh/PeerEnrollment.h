#pragma once
#include <cstdint>
#include "Enrollment.h"
#include "PeerRegistry.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

// Bridges PeerRegistry + Enrollment + MeshTransport::registerPeerWithEspNow (round 2
// task 10) — free functions, same reasoning as E2EKeyLookup.h (task 9).

// Add or rekey a peer with a known public key. allowRekey=false (over-the-air
// JOIN_ACK) never replaces an established key; allowRekey=true (server-approved
// enrollment) may. Returns false if the registry is full and the peer could not
// be added.
bool registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey,
                         PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode);

// Optional UI/app-triggered peer add.
void addPeer(const uint8_t* mac, PeerRegistry& peers);

// Outer JOIN_ACK dispatch: relay (via MeshTransport::sendBroadcast) if not addressed
// to this device; else delegate to enrollment.processJoinAck(...).
void dispatchJoinAck(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                     Enrollment& enrollment, RegisterPeerFn registerFn);

} // namespace mesh
} // namespace lattice
