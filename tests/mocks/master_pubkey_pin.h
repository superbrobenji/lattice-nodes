#pragma once
#include <cstdint>
// Fixed test values used across all host-tests. Do NOT match production values.
// MASTER_PUBKEY (Task 4, #42): must equal tests/e2e/harness/MasterKeypairFixture.h's
// MASTER_PUBLIC_KEY exactly — the e2e sim seeds its master node(s) with that
// fixture's (priv, pub) keypair so their real, mesh-advertised pubkey matches this
// pin (see SimNode::boot()'s seedPrivateKey32/seedPublicKey32 handling). Unit tests
// only ever reference this symbol (lattice::mesh::pin::MASTER_PUBKEY), never the
// literal bytes, so this value is free to change independent of them.
namespace lattice { namespace mesh { namespace pin {
constexpr uint8_t MASTER_PUBKEY[32] = {
    0x19, 0xBB, 0x35, 0x72, 0xA2, 0x2F, 0x52, 0x49,
    0x3A, 0x09, 0x57, 0xB1, 0x56, 0x23, 0x23, 0x24,
    0x66, 0x36, 0x67, 0x5E, 0x33, 0x42, 0xA4, 0xBE,
    0x41, 0xAC, 0xBA, 0x36, 0xF0, 0x70, 0x11, 0x32,
};
constexpr uint8_t MASTER_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 };
}}}
