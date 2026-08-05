#pragma once
#include <cstdint>
#include <cstring>
#include <esp_wifi.h>

// Canonical "read this node's own station MAC" helper, header-only.
//
// Consolidates 3 identical `static void readOwnMac(uint8_t[6])` copies that used to live in
// PirAdapter.cpp, SerialAdapter.cpp and SerialFraming.cpp (post-Phase-G audit item N). One
// definition, one place to change if the interface (WIFI_IF_STA) ever does.
//
// Post-Phase-G audit item F: the station MAC never changes at runtime, but
// esp_wifi_get_mac() is a syscall — Adapter.cpp and the callers below used to
// re-issue it on every RX frame (up to once per received mesh message). Cache
// it once, at boot, in g_deviceMac; every subsequent readOwnMac() call is a
// 6-byte memcpy instead of a syscall. Mesh::readMacAddress() (the existing
// single source of truth for the device MAC, Mesh.cpp) calls cacheDeviceMac()
// right after its own esp_wifi_get_mac() succeeds, so the cache is primed
// before any adapter's first RX. If something calls readOwnMac() before that
// (shouldn't happen given boot order, but cheap to guard), it falls back to a
// direct syscall and opportunistically caches the result.
//
// UNIT_TEST carve-out: the host test harness runs several *independent*
// simulated nodes in one process by swapping a single global mock MAC
// (esp_wifi_mock.h's mockDeviceMac) in and out per node (see
// tests/e2e/harness/NodeContext.cpp swapIn/swapOut), and test_pir_adapter.cpp
// changes mockDeviceMac mid-test to assert readOwnMac() reflects the CURRENT
// value. A process-lifetime cache here would freeze every simulated node at
// whichever MAC happened to be active on the first readOwnMac() call,
// silently breaking that isolation. So under UNIT_TEST, always re-query —
// the perf win is a firmware (real hardware) concern only; host tests don't
// pay a syscall cost either way.

namespace lattice {
namespace hw {

#ifndef UNIT_TEST

namespace detail {
inline uint8_t g_deviceMac[6] = {0, 0, 0, 0, 0, 0};
inline bool g_deviceMacCached = false;
} // namespace detail

// Prime the boot-time cache. Called once from Mesh::readMacAddress().
inline void cacheDeviceMac(const uint8_t mac[6]) {
  memcpy(detail::g_deviceMac, mac, 6);
  detail::g_deviceMacCached = true;
}

inline void readOwnMac(uint8_t out[6]) {
  if (detail::g_deviceMacCached) {
    memcpy(out, detail::g_deviceMac, 6);
    return;
  }
  esp_wifi_get_mac(WIFI_IF_STA, out);
  cacheDeviceMac(out);
}

#else // UNIT_TEST

// No caching on host builds — see carve-out note above.
inline void cacheDeviceMac(const uint8_t[6]) {}

inline void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

#endif // UNIT_TEST

} // namespace hw
} // namespace lattice
