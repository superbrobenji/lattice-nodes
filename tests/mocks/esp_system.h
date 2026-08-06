// Mock esp_system.h — shadows the ESP32 SDK header
#pragma once
#include <cstdint>
#include "Arduino.h" // for ESP mock — esp_restart() mirrors ESP.restart()'s flag below

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_EXT = 2,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
  ESP_RST_INT_WDT = 5,
  ESP_RST_TASK_WDT = 6,
  ESP_RST_WDT = 7,
  ESP_RST_DEEPSLEEP = 8,
  ESP_RST_BROWNOUT = 9,
  ESP_RST_SDIO = 10,
} esp_reset_reason_t;

inline esp_reset_reason_t esp_reset_reason() {
  return ESP_RST_POWERON;
}

// esp_restart() — Phase I Task 10 prereq cleanup replaced Arduino's
// ESP.restart() call sites with the native equivalent. Host/e2e tests
// observe restarts via ESP._restartRequested (see NodeContext.cpp /
// SimNode.cpp), so route through the same flag here to keep test-visible
// behavior identical to before the swap.
inline void esp_restart() {
  ESP._restartRequested = true;
}
