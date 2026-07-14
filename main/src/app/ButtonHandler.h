#pragma once
#include <Arduino.h>
#include "src/hardware/input/Button.h"
#include "src/hardware/output/Led.h"
#include "src/mesh/Mesh.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace app {

struct ButtonHandler {
  static constexpr unsigned long HOLD_MS = 5000;

  static void tick(lattice::hardware::Button& configBtn, lattice::hardware::Button& resetBtn,
                   lattice::mesh::Mesh& mesh, lattice::utils::EepromManager& em,
                   lattice::hardware::Led& greenLed, lattice::hardware::Led& redLed, bool isDevMode,
                   bool& devMasterFlag) {
    tickConfig(configBtn, mesh, em, greenLed, isDevMode, devMasterFlag);
    tickReset(resetBtn, em, greenLed, redLed);
  }

private:
  static void tickConfig(lattice::hardware::Button& btn, lattice::mesh::Mesh& mesh,
                         lattice::utils::EepromManager& em, lattice::hardware::Led& greenLed,
                         bool isDevMode, bool& devMasterFlag) {
    static bool wasPressed = false;
    static unsigned long holdStart = 0;

    if (btn.isPressed()) {
      if (!wasPressed) {
        wasPressed = true;
        holdStart = millis();
      } else if (millis() - holdStart >= HOLD_MS) {
        wasPressed = false; // Reset BEFORE action to prevent re-fire
        if (isDevMode) {
          bool newMaster = !mesh.getIsMaster();
          mesh.setIsMaster(newMaster);
          devMasterFlag = newMaster;
          Logger::logln("MAIN",
                        String("DEV MODE: Role toggled. Now ") + (newMaster ? "MASTER" : "NODE"),
                        LogLevel::LOG_INFO);
          greenLed.blink(newMaster ? 3 : 2, 150, 150);
        } else {
          bool wasMaster = em.loadMasterFlag();
          bool newMaster = !wasMaster;
          em.saveMasterFlag(newMaster);
          Logger::logln("MAIN",
                        String("Button held 5s: CONFIG TOGGLED. Now ") +
                            (newMaster ? "MASTER" : "NODE"),
                        LogLevel::LOG_INFO);
          Logger::logln("MAIN", "Restarting in 2 seconds for new role...", LogLevel::LOG_INFO);
          greenLed.blink(newMaster ? 3 : 2, 200, 200);
          delay(2000);
          em.forceFlush();
          ESP.restart();
        }
      }
    } else {
      wasPressed = false;
    }
  }

  static void tickReset(lattice::hardware::Button& btn, lattice::utils::EepromManager& em,
                        lattice::hardware::Led& greenLed, lattice::hardware::Led& redLed) {
    static bool wasPressed = false;
    static unsigned long holdStart = 0;
    static bool confirmPending = false;
    static uint32_t confirmDeadline = 0;

    if (btn.isPressed()) {
      if (!wasPressed) {
        wasPressed = true;
        holdStart = millis();
      } else if (millis() - holdStart >= HOLD_MS) {
        wasPressed = false;
        if (!confirmPending) {
          confirmPending = true;
          confirmDeadline = millis() + 3000;
          Logger::logln("MAIN", "Reset armed: hold again within 3s to confirm EEPROM wipe",
                        LogLevel::LOG_WARN);
          redLed.blink(3, 100, 100);
        } else if (millis() < confirmDeadline) {
          confirmPending = false;
          Logger::logln("MAIN", "EEPROM wipe confirmed. Clearing all...", LogLevel::LOG_WARN);
          em.clearAll();
          redLed.blink(5, 100, 100);
          greenLed.blink(5, 100, 100);
          delay(3000);
          em.forceFlush();
          ESP.restart();
        }
      }
    } else {
      wasPressed = false;
      if (confirmPending && millis() > confirmDeadline) {
        confirmPending = false;
        Logger::logln("MAIN", "Reset confirmation timed out", LogLevel::LOG_INFO);
      }
    }
  }
};

} // namespace app
} // namespace lattice
