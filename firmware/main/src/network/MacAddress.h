#ifndef PLANETOPA_MACADDRESS_H
#define PLANETOPA_MACADDRESS_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "MacEq.h"

namespace lattice {
namespace utils {

struct MacAddress {
  uint8_t bytes[6]{};
  // Constructors
  MacAddress() { memset(bytes, 0, 6); }
  explicit MacAddress(const uint8_t* mac) { memcpy(bytes, mac, 6); }

  // Comparison operators
  bool operator==(const MacAddress& other) const { return lattice::mac::eq(bytes, other.bytes); }
  bool operator!=(const MacAddress& other) const { return !(*this == other); }

  // Utility. Phase I Task 7 (XX): returns via a caller-provided fixed buffer
  // (18 bytes: "aa:bb:cc:dd:ee:ff" + NUL) instead of a heap-touching String,
  // matching Logger's char*-only signature. No callers exist yet in this
  // codebase; kept for future debug/log call sites.
  void toString(char out[18]) const {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5]);
  }
  bool isZero() const {
    for (auto b : bytes)
      if (b != 0)
        return false;
    return true;
  }
  bool isBroadcast() const {
    for (auto b : bytes)
      if (b != 0xFF)
        return false;
    return true;
  }
  void setBroadcast() { memset(bytes, 0xFF, 6); }
  void setZero() { memset(bytes, 0, 6); }
};

} // namespace utils
} // namespace lattice

#endif
