#pragma once
#include <cstdint>
#include <cstring>

// Canonical 6-byte MAC-address equality check (post-Phase-G audit item Q).
//
// Consolidates two competing idioms scattered across the mesh/adapter code:
//   memcmp(a, b, 6) == 0                                              (~30 sites)
//   lattice::utils::MacAddress(a) == lattice::utils::MacAddress(b)    (~13 sites,
//     strictly worse than the memcmp form — constructs two temporary
//     MacAddress objects, i.e. two extra 6-byte memcpys, just to compare)
// One definition, one place to change if the comparison ever needs to.
// lattice::utils::MacAddress::operator== (network/MacAddress.h) is
// implemented in terms of this helper so there's exactly one memcmp in the
// whole codebase.

namespace lattice {
namespace mac {

inline bool eq(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

} // namespace mac
} // namespace lattice
