#ifndef PLANETOPA_MACADDRESS_H
#define PLANETOPA_MACADDRESS_H

#include <Arduino.h>
#include <cstring>

namespace lattice {
namespace utils {

struct MacAddress {
  uint8_t bytes[6]{};
  // Constructors
  MacAddress() { memset(bytes, 0, 6); }
  explicit MacAddress(const uint8_t* mac) { memcpy(bytes, mac, 6); }
  explicit MacAddress(const String& macString);

  // Comparison operators
  bool operator==(const MacAddress& other) const { return memcmp(bytes, other.bytes, 6) == 0; }
  bool operator!=(const MacAddress& other) const { return !(*this == other); }

  // Utility
  String toString() const {
    char buf[18];
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
            bytes[5]);
    return String(buf);
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

inline MacAddress::MacAddress(const String& macString) {
  // Expect format "AA:BB:CC:DD:EE:FF"
  int vals[6];
  if (sscanf(macString.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &vals[0], &vals[1], &vals[2],
             &vals[3], &vals[4], &vals[5]) == 6) {
    for (int i = 0; i < 6; ++i)
      bytes[i] = static_cast<uint8_t>(vals[i]);
  } else {
    setZero();
  }
}

} // namespace utils
} // namespace lattice

#endif
