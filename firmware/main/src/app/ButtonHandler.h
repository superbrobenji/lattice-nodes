#pragma once
#include <Arduino.h>
#include <cstdio>
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
                   lattice::mesh::Mesh& mesh, lattice::hardware::Led& greenLed,
                   lattice::hardware::Led& redLed, bool isDevMode, bool& devMasterFlag) {
    tickConfig(configBtn, mesh, greenLed, isDevMode, devMasterFlag);
    tickReset(resetBtn, greenLed, redLed);
  }

private:
  static void tickConfig(lattice::hardware::Button& btn, lattice::mesh::Mesh& mesh,
                         lattice::hardware::Led& greenLed, bool isDevMode, bool& devMasterFlag) {
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
          LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO, "DEV MODE: Role toggled. Now %s",
                       newMaster ? "MASTER" : "NODE");
          greenLed.blink(newMaster ? 3 : 2, 150, 150);
        } else {
          bool wasMaster = lattice::eeprom::loadMasterFlag();
          bool newMaster = !wasMaster;
          lattice::eeprom::saveMasterFlag(newMaster);
          LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO,
                       "Button held 5s: CONFIG TOGGLED. Now %s", newMaster ? "MASTER" : "NODE");
          LATTICE_LOGLN("MAIN", "Restarting in 2 seconds for new role...",
                        lattice::utils::LogLevel::LOG_INFO);
          greenLed.blink(newMaster ? 3 : 2, 200, 200);
          delay(2000);
          lattice::eeprom::forceFlush();
          ESP.restart();
        }
      }
    } else {
      wasPressed = false;
    }
  }

  static void tickReset(lattice::hardware::Button& btn, lattice::hardware::Led& greenLed,
                        lattice::hardware::Led& redLed) {
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
          LATTICE_LOGLN("MAIN", "Reset armed: hold again within 3s to confirm EEPROM wipe",
                        lattice::utils::LogLevel::LOG_WARN);
          redLed.blink(3, 100, 100);
        } else if (millis() < confirmDeadline) {
          confirmPending = false;
          LATTICE_LOGLN("MAIN", "EEPROM wipe confirmed. Clearing all...",
                        lattice::utils::LogLevel::LOG_WARN);
          lattice::eeprom::clearAll();
          redLed.blink(5, 100, 100);
          greenLed.blink(5, 100, 100);
          delay(3000);
          lattice::eeprom::forceFlush();
          ESP.restart();
        }
      }
    } else {
      wasPressed = false;
      if (confirmPending && millis() > confirmDeadline) {
        confirmPending = false;
        LATTICE_LOGLN("MAIN", "Reset confirmation timed out", lattice::utils::LogLevel::LOG_INFO);
      }
    }
  }
};

} // namespace app
} // namespace lattice
