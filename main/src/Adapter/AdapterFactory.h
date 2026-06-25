#ifndef ADAPTER_FACTORY_H
#define ADAPTER_FACTORY_H

#include "Adapter.h"
#include <Arduino.h>

namespace planetopia {
namespace adapter {

// Default pins for each adapter type
static constexpr int PIR_ADAPTER_DEFAULT_PIN = 27;    // PIR sensor pin
static constexpr int WIFI_ADAPTER_DEFAULT_PIN = -1;   // WiFi doesn't need a pin
static constexpr int LED_ADAPTER_DEFAULT_PIN = 2;     // Built-in LED pin
static constexpr int SERIAL_ADAPTER_DEFAULT_PIN = -1; // Serial doesn't need a pin

class AdapterFactory {
public:
  // Create adapter with specified type and pin
  static Adapter* createAdapter(adapter_types type, int pin);

  // Create adapter from EEPROM (automatically uses correct pin for adapter type)
  static Adapter* createFromEEPROM();

  // Load adapter type from EEPROM
  static adapter_types loadAdapterTypeFromEEPROM();

  // Save adapter type to EEPROM
  static void saveAdapterTypeToEEPROM(adapter_types type);

  // Initialize EEPROM defaults if not set
  static void initializeDefaultsIfUnset();

  // Get the default pin for a specific adapter type
  static int getDefaultPinForAdapter(adapter_types type);

  // Set dev mode flag (bypasses EEPROM operations)
  static void setDevMode(bool isDev);

  // Named conversion functions for EEPROM
  static adapter_types adapterTypeFromEEPROM(uint8_t raw);
  static uint8_t adapterTypeToEEPROM(adapter_types type);

private:
  static bool isDevMode_;
};

} // namespace adapter
} // namespace planetopia
#endif
