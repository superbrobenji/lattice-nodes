// Mock esp_random.h — shadows the ESP-IDF header for host test builds.
// lattice::crypto (firmware/main/src/crypto/Crypto.h) draws all randomness
// through esp_fill_random(); on target that is the hardware TRNG, here a
// seeded mt19937_64 (host tests only — never a crypto-quality source, never
// used for anything persisted).
#pragma once
#include <cstddef>
#include <cstdint>
#include <random>

inline void esp_fill_random(void* buf, size_t len) {
  static std::mt19937_64 rng{std::random_device{}()};
  auto* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) {
    p[i] = static_cast<uint8_t>(rng());
  }
}

inline uint32_t esp_random(void) {
  uint32_t v = 0;
  esp_fill_random(&v, sizeof(v));
  return v;
}
