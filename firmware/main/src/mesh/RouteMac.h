#pragma once
#include <cstdint>
#include <cstring>
#include "src/crypto/Crypto.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

// Route-report chain MAC (Phase C, spec §4 / issue #44, header-only, mirrors
// E2ECrypto.h pattern).
//
// Authenticates the plaintext route_path/route_len header fields a route
// report accumulates hop-by-hop on its way to the master. Each hop chains a
// truncated HMAC-SHA256 into msg.auth_path, keyed off that hop's own pairwise
// k_up with the master (existing E2E key material — no new keys, no server
// participation, no new persisted state). The master reconstructs the same
// chain from each hop's k_up and drops the frame on mismatch.
//
// This only authenticates uplink route-report accumulation — no downlink
// frame MAC, no hub-side verification (see design doc "Non-goals").

namespace lattice {
namespace mesh {
namespace routemac {

// hop_context (30 bytes):
//   origin_mac(6) || dest_mac(6) || epoch(4LE) || seq(2LE) || prev_hop_mac(6) || this_hop_mac(6)
// prev_hop_mac is zeroed for the originating hop.
constexpr size_t HOP_CTX_LEN = 30;
constexpr size_t AUTH_PATH_LEN = 8;

// Phase I Task 7 (SS): byte-by-byte little-endian shift+mask+store replaced
// with memcpy (see E2ECrypto.h::buildNonce for the alignment rationale —
// mesh_message is packed, and memcpy is the alignment-safe way to copy a
// multi-byte field out of it on Xtensa). ESP32 is little-endian, so this
// reproduces the prior output bit-for-bit.
inline void buildHopContext(const mesh_message& msg, const uint8_t* prev_hop,
                            const uint8_t* this_hop, uint8_t* out_ctx) {
  uint8_t* p = out_ctx;
  memcpy(p, msg.origin_mac_address, 6);
  p += 6;
  memcpy(p, msg.target_mac_address, 6);
  p += 6;
  memcpy(p, &msg.epoch_num, sizeof(msg.epoch_num));
  p += sizeof(msg.epoch_num);
  memcpy(p, &msg.seq_num, sizeof(msg.seq_num));
  p += sizeof(msg.seq_num);
  memcpy(p, prev_hop, 6);
  p += 6;
  memcpy(p, this_hop, 6);
}

// mac_i = HMAC-SHA256(secret, hop_context_i || mac_{i-1})[:8]
// For the originating hop, prev_mac must be zeroed 8B.
// Tiger-Style: stack-only, fixed-size buffers, no heap, no dynamic length.
//
// Phase J: HMAC via lattice::crypto::hmac_sha256 (mbedtls one-shot).
inline void chainStep(const uint8_t* secret, const uint8_t* hop_ctx, const uint8_t* prev_mac,
                      uint8_t* out_mac) {
  uint8_t input[HOP_CTX_LEN + AUTH_PATH_LEN];
  memcpy(input, hop_ctx, HOP_CTX_LEN);
  memcpy(input + HOP_CTX_LEN, prev_mac, AUTH_PATH_LEN);

  uint8_t full[32]; // SHA-256 output
  if (!lattice::crypto::hmac_sha256(secret, 32, input, sizeof(input), full)) {
    // Fail-safe: an unwritable MAC must not leak garbage — zero it so the
    // frame fails verification at the master and gets dropped.
    memset(out_mac, 0, AUTH_PATH_LEN);
    return;
  }
  memcpy(out_mac, full, AUTH_PATH_LEN); // truncate to first 8 bytes
}

} // namespace routemac
} // namespace mesh
} // namespace lattice
