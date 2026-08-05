#pragma once
#include <cstdint>
#include <esp_wifi.h>

// Canonical "read this node's own station MAC" helper, header-only.
//
// Consolidates 3 identical `static void readOwnMac(uint8_t[6])` copies that used to live in
// PirAdapter.cpp, SerialAdapter.cpp and SerialFraming.cpp (post-Phase-G audit item N). One
// definition, one place to change if the interface (WIFI_IF_STA) ever does.

namespace lattice {
namespace hw {

inline void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

} // namespace hw
} // namespace lattice
