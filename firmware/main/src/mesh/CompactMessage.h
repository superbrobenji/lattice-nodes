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
// costs while it sits in Mesh::recvQueue or is carried on the stack past the
// initial decode boundary (Mesh::onDataRecvCallback / Mesh::drainRecvQueue).
//
// Field-drop rationale (audited against every recvQueue-reachable consumer in
// Mesh.cpp/Enrollment.cpp before finalizing — see task-3-report.md for the
// full trace): the design doc's original sketch for this struct additionally
// dropped route_len/route_path/auth_tag/auth_path/enrollment_public_key, but
// each of those is read by at least one message type that flows through
// Mesh::recvQueue in normal (not just edge-case) operation:
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
// What IS safe to drop: secondary_master_mac[6] + secondary_public_key[32]
// (JOIN_ACK dual-master fields, Phase 4). No current test — unit or e2e —
// exercises these fields via the real receive pipeline (dual-master e2e
// coverage TOFU-learns the secondary from its BEACON, not from JOIN_ACK's
// secondary fields; see tests/e2e/scenarios/test_dual_master_e2e.cpp's
// top-of-file comment: full DATA failover via JOIN_ACK-conveyed secondary
// keying is already a documented, pre-existing gap, not a working, tested
// path). toWire() zeroes them on reconstruction — a Mesh::recvQueue frame
// carrying a real dual-master JOIN_ACK's secondary fields over the air will
// not propagate them past the compact boundary; this is a known, narrow,
// test-verified-safe limitation of this optimization, not a new regression
// of a working feature.
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
  // Skipped (see rationale above): secondary_master_mac[6], secondary_public_key[32]
  // (protocol v0.6.0 dropped these top-level wire fields entirely — see
  // Enrollment::processJoinAck, which now reads the JOIN_ACK secondary from
  // data[4..42] instead).
};
// CompactMessage carries every field mesh_message still has except
// secondary_master_mac/secondary_public_key, which no longer exist on the
// wire at all as of protocol v0.6.0 (they moved into JOIN_ACK's data[]
// payload — see Enrollment::processJoinAck). Not currently used for Mesh::recvQueue
// (see Mesh.h's RecvQueueEntry comment) — kept at <= 220 so a future consumer
// noticing this doesn't have to re-derive the achievable budget from scratch.
static_assert(sizeof(CompactMessage) <= 220, "CompactMessage residency budget");

// Convert wire -> compact for enqueue. Copies every field CompactMessage
// declares; secondary_master_mac/secondary_public_key are intentionally not
// read (there is nowhere to put them).
void toCompact(const mesh_message& src, CompactMessage& dst);

// Convert compact -> wire for reconstruction (e.g. drainRecvQueue dispatch,
// or re-sending a previously-queued frame). Fields CompactMessage does not
// carry (secondary_master_mac, secondary_public_key) are zeroed in dst.
void toWire(const CompactMessage& src, mesh_message& dst);

} // namespace mesh
} // namespace lattice
