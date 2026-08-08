#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

// Compact in-RAM representation of mesh_message for buffered/queued processing
// (Phase G §7, RAM residency). The wire form (mesh_message, 200B as of the
// protocol v0.6.0 wire shrink, §8 — was 250B when this type was designed)
// remains the source of truth for anything that touches the radio
// (SerialFraming / ESP-NOW send path) — this type only shrinks what a frame
// costs while it sits in MeshTransport::recvQueue or is carried on the stack
// past the initial decode boundary (MeshTransport::onDataRecvCallback /
// Mesh::handleReceivedMessage, Phase B Task 4 — formerly Mesh::drainRecvQueue).
//
// Field-drop rationale (audited against every recvQueue-reachable consumer in
// Mesh.cpp/Enrollment.cpp before finalizing — see task-3-report.md for the
// full trace): the design doc's original sketch for this struct additionally
// dropped route_len/route_path/auth_tag/auth_path/enrollment_public_key, but
// each of those is read by at least one message type that flows through
// MeshTransport::recvQueue in normal (not just edge-case) operation:
//   - auth_tag:               every AEAD open/seal (E2ECrypto.h) — sealed
//                              ADAPTER_DATA/ROUTE_REPORT local delivery.
//   - route_len/route_path:   downlink source-routed relay (processAdapterData,
//                              spec §4's explicitly "stateless" relay — the
//                              path MUST travel in the frame, not be
//                              reconstructed from local state) AND
//                              ROUTE_REPORT verify/relay/record.
//   - auth_path:               ROUTE_REPORT chain-MAC verify/extend.
//   - enrollment_public_key:   ENROLLMENT (JOIN_REQUEST) relay/registration
//                              AND JOIN_ACK master-pubkey-pin check.
// Dropping any of those from the queued representation is a correctness
// regression (confirmed by tracing tests/e2e/scenarios/test_multihop_e2e.cpp's
// MasterSendsSealedConfigSetThroughRelayToLeaf, which exercises the real
// ISR->queue->drain pipeline for a downlink relay + AEAD open).
//
// secondary_master_mac[6] + secondary_public_key[32] (JOIN_ACK dual-master
// fields, Phase 4) have no dedicated slot in this struct — but as of
// protocol v0.6.0 (wire shrink §8) they no longer have a dedicated slot on
// the wire either. They were folded into mesh_message::data[64] at
// data[4..42] (see SerialFraming::decode / Enrollment::processJoinAck).
// Because toCompact()/toWire() copy data[64] verbatim (CompactMessage.cpp),
// this secondary data DOES survive the compact round trip today: a
// MeshTransport::recvQueue frame carrying a real dual-master JOIN_ACK's
// secondary fields keeps them, embedded in dst.data, all the way through
// Mesh::handleReceivedMessage's reconstruction. There is nothing to "drop"
// here anymore — readers just need to know to pull the secondary MAC/pubkey
// out of data[4..42] on JOIN_ACK frames, same as the wire form requires.
struct CompactMessage {
  int32_t data_type;
  uint32_t epoch_num;
  uint8_t origin_mac[6];
  uint8_t target_mac[6];
  uint8_t last_hop_mac[6];
  uint16_t seq_num;
  uint8_t proto_version;
  uint8_t message_type;
  uint8_t hop_count;
  uint8_t route_len;
  uint8_t data[64];
  uint8_t enrollment_public_key[32];
  // Mirrors mesh_message::route_path, which protocol v0.6.0 (wire shrink §8)
  // shrank 60→48 (MAX_HOPS 10→8). Must track the wire size exactly — toWire()
  // memcpy's sizeof(this field) into mesh_message::route_path[48].
  uint8_t route_path[48];
  uint8_t auth_tag[16];
  uint8_t auth_path[8];
  // No dedicated secondary_master_mac[6] / secondary_public_key[32] fields
  // (see header comment above): protocol v0.6.0 dropped these as top-level
  // wire fields entirely — see Enrollment::processJoinAck, which reads the
  // JOIN_ACK secondary from data[4..42] instead. That data lives inside
  // `data[64]` below and round-trips with it.
};
// CompactMessage has no dedicated field for secondary_master_mac/
// secondary_public_key, which no longer exist as top-level wire fields as
// of protocol v0.6.0 (they moved into JOIN_ACK's data[] payload — see
// Enrollment::processJoinAck). They are still present, opaquely, inside
// `data[64]` below, which toCompact()/toWire() copy verbatim. Not currently
// used for Mesh::recvQueue (see Mesh.h's RecvQueueEntry comment) — kept at
// <= 220 so a future consumer noticing this doesn't have to re-derive the
// achievable budget from scratch.
static_assert(sizeof(CompactMessage) <= 220, "CompactMessage residency budget");

// Convert wire -> compact for enqueue. Copies every field CompactMessage
// declares, including data[64] verbatim — which carries JOIN_ACK's
// secondary_master_mac/secondary_public_key at data[4..42] along with it.
void toCompact(const mesh_message& src, CompactMessage& dst);

// Convert compact -> wire for reconstruction (e.g. drainRecvQueue dispatch,
// or re-sending a previously-queued frame). data[64] (including any
// JOIN_ACK secondary_master_mac/secondary_public_key at data[4..42]) is
// copied through verbatim from src.
void toWire(const CompactMessage& src, mesh_message& dst);

} // namespace mesh
} // namespace lattice
