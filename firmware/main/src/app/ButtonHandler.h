#pragma once
#include <cstdio>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "src/hardware/input/Button.h"
#include "src/hardware/output/Led.h"
#include "src/mesh/Mesh.h"
#include "src/persistence/eeprom/EepromCore.h"
#include "src/persistence/eeprom/EepromRole.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace app {

struct ButtonHandler {
  static constexpr uint64_t HOLD_MS = 5000;

  static void tick(lattice::hardware::Button& configBtn, lattice::hardware::Button& resetBtn,
                   lattice::mesh::Mesh& mesh, lattice::hardware::Led& greenLed,
                   lattice::hardware::Led& redLed, bool isDevMode, bool& devMasterFlag) {
    tickConfig(configBtn, mesh, greenLed, isDevMode, devMasterFlag);
    tickReset(resetBtn, greenLed, redLed);
  }

private:
  // Phase I Task 7 (WW): Led::pulse()/update() are non-blocking — the two
  // call sites below specifically need the pattern to finish playing out on
  // the physical LED(s) before an imminent restart (previously guaranteed by
  // blink()'s internal blocking delay()), so they pump update() locally in a
  // small loop instead of relying on the main loop's per-iteration pump.
  static void pumpLedsUntilIdle(lattice::hardware::Led& a, lattice::hardware::Led* b = nullptr) {
    while (true) {
      uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
      a.update(now);
      if (b)
        b->update(now);
      if (!a.isBusy() && !(b && b->isBusy()))
        break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  static void tickConfig(lattice::hardware::Button& btn, lattice::mesh::Mesh& mesh,
                         lattice::hardware::Led& greenLed, bool isDevMode, bool& devMasterFlag) {
    static bool wasPressed = false;
    static uint64_t holdStart = 0;

    if (btn.isPressed()) {
      if (!wasPressed) {
        wasPressed = true;
        holdStart = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
      } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - holdStart >= HOLD_MS) {
        wasPressed = false; // Reset BEFORE action to prevent re-fire
        if (isDevMode) {
          bool newMaster = !mesh.getIsMaster();
          mesh.setIsMaster(newMaster);
          devMasterFlag = newMaster;
          LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO, "DEV MODE: Role toggled. Now %s",
                       newMaster ? "MASTER" : "NODE");
          greenLed.pulse(newMaster ? 3 : 2, 150, 150);
        } else {
          bool wasMaster = lattice::eeprom::loadMasterFlag();
          bool newMaster = !wasMaster;
          lattice::eeprom::saveMasterFlag(newMaster);
          LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO,
                       "Button held 5s: CONFIG TOGGLED. Now %s", newMaster ? "MASTER" : "NODE");
          LATTICE_LOGLN("MAIN", "Restarting in 2 seconds for new role...",
                        lattice::utils::LogLevel::LOG_INFO);
          greenLed.pulse(newMaster ? 3 : 2, 200, 200);
          pumpLedsUntilIdle(greenLed);
          vTaskDelay(pdMS_TO_TICKS(2000));
          lattice::eeprom::forceFlush();
          esp_restart();
        }
      }
    } else {
      wasPressed = false;
    }
  }

  static void tickReset(lattice::hardware::Button& btn, lattice::hardware::Led& greenLed,
                        lattice::hardware::Led& redLed) {
    static bool wasPressed = false;
    static uint64_t holdStart = 0;
    static bool confirmPending = false;
    static uint64_t confirmDeadline = 0;

    if (btn.isPressed()) {
      if (!wasPressed) {
        wasPressed = true;
        holdStart = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
      } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - holdStart >= HOLD_MS) {
        wasPressed = false;
        if (!confirmPending) {
          confirmPending = true;
          confirmDeadline = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL + 3000;
          LATTICE_LOGLN("MAIN", "Reset armed: hold again within 3s to confirm EEPROM wipe",
                        lattice::utils::LogLevel::LOG_WARN);
          redLed.pulse(3, 100, 100);
        } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL < confirmDeadline) {
          confirmPending = false;
          LATTICE_LOGLN("MAIN", "EEPROM wipe confirmed. Clearing all...",
                        lattice::utils::LogLevel::LOG_WARN);
          lattice::eeprom::clearAll();
          redLed.pulse(5, 100, 100);
          greenLed.pulse(5, 100, 100);
          pumpLedsUntilIdle(redLed, &greenLed);
          vTaskDelay(pdMS_TO_TICKS(3000));
          lattice::eeprom::forceFlush();
          esp_restart();
        }
      }
    } else {
      wasPressed = false;
      if (confirmPending &&
          static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL > confirmDeadline) {
        confirmPending = false;
        LATTICE_LOGLN("MAIN", "Reset confirmation timed out", lattice::utils::LogLevel::LOG_INFO);
      }
    }
  }
};

} // namespace app
} // namespace lattice
