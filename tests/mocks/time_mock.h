#pragma once
#include <cstdint>

// Controllable mock clock — call advanceMillis() in tests to simulate time passing
extern uint32_t _mockMillis;
inline uint32_t millis() { return _mockMillis; }
inline void advanceMillis(uint32_t ms) { _mockMillis += ms; }
inline void resetMillis() { _mockMillis = 0; }
inline void delay(uint32_t) {}  // no-op in tests
