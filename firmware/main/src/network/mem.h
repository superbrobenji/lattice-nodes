#pragma once
#include <cstddef>
#include <cstdint>

// lattice::mem::is_zero (Phase H2 audit item Z): dedups the "all-zero
// sentinel" check that Enrollment.cpp, E2EKeyStore.h, and Mesh.cpp each
// hand-rolled independently (6B secondary-MAC and 32B public-key variants).

namespace lattice {
namespace mem {

// True iff all n bytes at p are zero.
inline bool is_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i] != 0)
      return false;
  return true;
}

} // namespace mem
} // namespace lattice
