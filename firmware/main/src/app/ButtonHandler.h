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
  // Shared "press-and-hold for holdMs" detection, factored out of
  // tickConfig/tickReset (both hand-rolled the identical skeleton). Returns
  // true exactly once — the instant the hold threshold is crossed — and
  // false every other tick (not-pressed, still-pressed-but-under-threshold,
  // or already-fired-and-released). wasPressed/holdStart are the caller's own
  // static state (each of tickConfig/tickReset keeps its own pair, matching
  // today's per-function static locals).
  static bool detectHold(lattice::hardware::Button& btn, uint64_t holdMs, bool& wasPressed,
                         uint64_t& holdStart) {
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    if (btn.isPressed()) {
      if (!wasPressed) {
        wasPressed = true;
        holdStart = now;
        return false;
      }
      if (now - holdStart >= holdMs) {
        wasPressed = false; // reset before firing, matches tickConfig's existing comment/behavior
        return true;
      }
      return false;
    }
    wasPressed = false;
    return false;
  }

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

    if (!detectHold(btn, HOLD_MS, wasPressed, holdStart)) {
      return;
    }

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

  static void tickReset(lattice::hardware::Button& btn, lattice::hardware::Led& greenLed,
                        lattice::hardware::Led& redLed) {
    static bool wasPressed = false;
    static uint64_t holdStart = 0;
    static bool confirmPending = false;
    static uint64_t confirmDeadline = 0;

    if (detectHold(btn, HOLD_MS, wasPressed, holdStart)) {
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
      return;
    }

    // detectHold's internal wasPressed=false covers the "still pressed, under
    // threshold" and "released mid-press" cases as well as "not pressed at
    // all" — the ORIGINAL code's timeout check was gated specifically on
    // !btn.isPressed() (it lived in the `else` of `if (btn.isPressed())`),
    // so it must be re-gated on that here too. Without the explicit
    // !btn.isPressed() check, continuing to hold the button past
    // confirmDeadline (e.g. a long second hold that crosses the 3s confirm
    // window before reaching the next HOLD_MS threshold) would incorrectly
    // reset confirmPending mid-hold and let the eventual second hold-fire
    // re-arm a fresh window instead of silently no-op'ing like the original.
    // btn.isPressed() is safe to call again here: Button::isPressed()
    // samples at most once per DEBOUNCE_DELAY_MS and this call lands in the
    // same tick (same millis()) as detectHold's internal call above, so it
    // returns the already-cached debounce result rather than re-sampling.
    if (!btn.isPressed() && confirmPending &&
        static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL > confirmDeadline) {
      confirmPending = false;
      LATTICE_LOGLN("MAIN", "Reset confirmation timed out", lattice::utils::LogLevel::LOG_INFO);
    }
  }
};

} // namespace app
} // namespace lattice
