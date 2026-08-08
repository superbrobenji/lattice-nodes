#pragma once
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "src/persistence/eeprom/EepromDiagnostics.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace app {

struct BootManager {
  static void check() {
    esp_reset_reason_t reason = esp_reset_reason();
    lattice::eeprom::saveRebootReason(static_cast<uint8_t>(reason));
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
      uint8_t count = lattice::eeprom::loadRebootCount();
      count++;
      lattice::eeprom::saveRebootCount(count);
      LATTICE_LOGF("BOOT", lattice::utils::LogLevel::LOG_INFO, "WDT reset #%d (reason: %d)", count,
                   (int)reason);
      if (count >= 5) {
        LATTICE_LOGLN("BOOT", "WDT loop detected — halting. Manual reset required.",
                      lattice::utils::LogLevel::LOG_WARN);
        while (true) {
          vTaskDelay(pdMS_TO_TICKS(1000));
        }
      }
    } else {
      lattice::eeprom::saveRebootCount(0);
    }
  }
};

} // namespace app
} // namespace lattice
