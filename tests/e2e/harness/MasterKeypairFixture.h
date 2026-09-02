#pragma once
// Phase D (#42): a fixed, self-consistent Curve25519 keypair for the e2e sim's
// master node(s). Enrollment::init() normally generates a fresh random keypair
// via mbedtls entropy every boot (see MeshCrypto.h::generateKeypair) — fine
// pre-pin, but the JOIN_ACK pubkey pin (Enrollment::processJoinAck) now rejects
// any master whose advertised pubkey isn't lattice::mesh::pin::MASTER_PUBKEY
// (tests/mocks/master_pubkey_pin.h). To keep e2e scenarios in the pin-active
// regime (preferred over bypassing the check — see task-4 brief), the harness
// seeds a master SimNode's EEPROM with THIS keypair before Enrollment::init()
// ever runs (see SimNode::boot(), NodeConfig::seedPrivateKey32/seedPublicKey32),
// so it loads it instead of generating a random one. MASTER_PUBLIC_KEY below
// must equal tests/mocks/master_pubkey_pin.h's MASTER_PUBKEY exactly.
//
// That identity (pin == the master board's own on-device key) is exactly what
// a real deployment must satisfy too (#126): tools/gen_master_pubkey_pin.py
// derives the pin from the board's serial LATTICE_PUBKEY: line, never from the
// hub's masterkey.json. tests/tools/test_gen_master_pubkey_pin.py checks the
// two committed fixtures agree byte-for-byte.
//
// MUST be a real (priv, pub) pair — priv's Curve25519 scalar-mult public point
// must equal pub — otherwise ECDH between the master and any enrolled node
// produces mismatched shared secrets and E2E-sealed traffic silently fails to
// decrypt. Generated once offline via the same mbedtls ECDH keygen path
// production uses (MeshCrypto.h::generateKeypair); not derived from anything
// secret, and not the production pin — safe to commit.
#include <cstdint>

namespace sim {
namespace fixture {

constexpr uint8_t MASTER_PRIVATE_KEY[32] = {
    0x58, 0xD6, 0x58, 0x65, 0xC1, 0x1E, 0x09, 0xCA, 0xB5, 0x64, 0x22, 0x8A,
    0xDE, 0xDA, 0x5C, 0xCA, 0x1E, 0x18, 0x37, 0x55, 0xD9, 0x7D, 0xEC, 0x59,
    0x23, 0x2D, 0x8F, 0x0D, 0xAE, 0x85, 0x53, 0x90,
};
// Must equal lattice::mesh::pin::MASTER_PUBKEY (tests/mocks/master_pubkey_pin.h).
constexpr uint8_t MASTER_PUBLIC_KEY[32] = {
    0x19, 0xBB, 0x35, 0x72, 0xA2, 0x2F, 0x52, 0x49, 0x3A, 0x09, 0x57, 0xB1,
    0x56, 0x23, 0x23, 0x24, 0x66, 0x36, 0x67, 0x5E, 0x33, 0x42, 0xA4, 0xBE,
    0x41, 0xAC, 0xBA, 0x36, 0xF0, 0x70, 0x11, 0x32,
};

} // namespace fixture
} // namespace sim
