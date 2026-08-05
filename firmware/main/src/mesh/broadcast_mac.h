#pragma once
#include <cstdint>

// Canonical ESP-NOW broadcast MAC (FF:FF:FF:FF:FF:FF), header-only.
//
// Consolidates the ~7 independent `static const uint8_t ...[6] = {0xFF, ...}` copies that used
// to live scattered across Mesh.cpp and Enrollment.cpp (post-Phase-G audit item E). One
// definition, one place to read/change.

namespace lattice {
namespace mesh {

constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

} // namespace mesh
} // namespace lattice
