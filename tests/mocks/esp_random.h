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

// The scalar esp_random() is intentionally only DECLARED (not defined) here
// — lattice::crypto (Crypto.h) only ever calls esp_fill_random() above.
// mocks/Arduino.cpp remains the sole DEFINITION of the scalar esp_random()
// mock (deterministic, used by MasterBeacon.cpp's relay jitter); defining it
// here too would be an ODR violation in any translation unit that ends up
// linking both mocks' object files into the same binary (see Phase J Task 2
// review). A plain declaration is safe: every host test/e2e target already
// links mocks/Arduino.cpp (see tests/CMakeLists.txt FIRMWARE_SOURCES), so
// this prototype resolves at link time against that TU's definition — this
// is what lets Phase C finding 17 (Logger off Arduino) route
// MasterBeacon.cpp's esp_random() call through the real <esp_random.h> path
// (mocked here) instead of the no-longer-transitive Arduino.h chain.
uint32_t esp_random(void);
