#pragma once
// Mock esp_timer.h — shadows the ESP-IDF esp_timer component header for host
// tests.
//
// Phase I Task 6 (FF): production code migrates every millis()-based timing
// site to esp_timer_get_time() / 1000ULL. Backing that with the SAME
// _mockMillis clock tests already drive via advanceMillis()/resetMillis()
// (time_mock.h) means no test changes are needed when a call site flips from
// millis() to esp_timer_get_time() — the mock clock still moves in lockstep.
#include <cstdint>
#include "time_mock.h"

extern "C" {
inline int64_t esp_timer_get_time(void) {
  return static_cast<int64_t>(_mockMillis) * 1000;
}
}
