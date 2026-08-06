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

// The scalar esp_random() is intentionally NOT mocked here — lattice::crypto
// (Crypto.h) only ever calls esp_fill_random() above. mocks/Arduino.h is the
// sole owner of the scalar esp_random() mock (deterministic, used by e.g.
// Mesh.cpp's relay jitter); defining it here too would be an ODR violation
// in any translation unit that ends up linking both mocks' object files into
// the same binary (see Phase J Task 2 review — two conflicting inline
// definitions of the same function is ill-formed, no diagnostic required,
// even if only one header is included per TU).
