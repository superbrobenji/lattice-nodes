#ifndef ADAPTER_FACTORY_H
#define ADAPTER_FACTORY_H

#include "Adapter.h"
#include <Arduino.h>

namespace lattice {
namespace adapter {

// Default pins for each adapter type (Phase G audit item L: int -> uint8_t; ESP32
// GPIO pins are 0-39). SERIAL_ADAPTER_DEFAULT_PIN's sentinel moves from -1 (not
// representable in uint8_t) to 255 — still outside the valid 0-39 GPIO range.
// LED_ADAPTER_DEFAULT_PIN removed in Phase G Task 2 alongside the LED stub.
static constexpr uint8_t PIR_ADAPTER_DEFAULT_PIN = 27;     // PIR sensor pin
static constexpr uint8_t SERIAL_ADAPTER_DEFAULT_PIN = 255; // Serial doesn't need a pin

class AdapterFactory {
public:
  // Create adapter with specified type and pin
  static Adapter* createAdapter(adapter_types type, uint8_t pin);

  // Create adapter from EEPROM (automatically uses correct pin for adapter type)
  static Adapter* createFromEEPROM();

  // Load adapter type from EEPROM
  static adapter_types loadAdapterTypeFromEEPROM();

  // Save adapter type to EEPROM
  static void saveAdapterTypeToEEPROM(adapter_types type);

  // Initialize EEPROM defaults if not set
  static void initializeDefaultsIfUnset();

  // Get the default pin for a specific adapter type
  static uint8_t getDefaultPinForAdapter(adapter_types type);

  // Set dev mode flag (bypasses EEPROM operations)
  static void setDevMode(bool isDev);

  // Named conversion functions for EEPROM
  static adapter_types adapterTypeFromEEPROM(uint8_t raw);
  static uint8_t adapterTypeToEEPROM(adapter_types type);

private:
  static bool isDevMode_;
};

} // namespace adapter
} // namespace lattice
#endif
