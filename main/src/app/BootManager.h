#pragma once
#include <esp_system.h>
#include <Arduino.h>
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace app {

struct BootManager {
  static void check(lattice::utils::EepromManager& em) {
    esp_reset_reason_t reason = esp_reset_reason();
    em.saveRebootReason(static_cast<uint8_t>(reason));
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
      uint8_t count = em.loadRebootCount();
      count++;
      em.saveRebootCount(count);
      Serial.printf("[BOOT] WDT reset #%d (reason: %d)\n", count, (int)reason);
      if (count >= 5) {
        Serial.println("[BOOT] WDT loop detected — halting. Manual reset required.");
        while (true) { delay(1000); }
      }
    } else {
      em.saveRebootCount(0);
    }
  }
};

} // namespace app
} // namespace lattice
