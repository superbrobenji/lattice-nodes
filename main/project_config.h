#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include <Arduino.h>
#include "src/core/Logger.h"
#include "src/Adapter/Adapter.h"

namespace planetopia {
namespace config {

// =====================
// 1. Build / Runtime Mode
// =====================
// Compile-time development switch. When true the firmware: skips EEPROM writes,
// uses DEFAULT_ADAPTER, and ignores persistent role settings. Set false for
// production flashes.
constexpr bool DEV_MODE = true;
// Node role to assume at boot when DEV_MODE is true
constexpr bool DEFAULT_DEV_MASTER = false;

// =====================
// 2. Default Behaviour
// =====================
// Adapter instantiated on first boot or in DEV_MODE
constexpr planetopia::adapter::adapter_types DEFAULT_ADAPTER = planetopia::adapter::PIR_ADAPTER;
// Primary mesh-beacon interval (milliseconds)
constexpr unsigned long MASTER_BEACON_INTERVAL_MS = 2000;

// =====================
// 3. Radio / ESP-NOW
// =====================
// Wi-Fi / ESP-NOW channel – ALL nodes must match
constexpr uint8_t WIFI_CHANNEL = 1;
// Global 16-byte AES key – ALWAYS used for ESP-NOW encryption
static constexpr uint8_t DEFAULT_MESH_KEY[16] = {
  0x12,0x34,0x56,0x78, 0x9A,0xBC,0xDE,0xF0,
  0x11,0x22,0x33,0x44, 0x55,0x66,0x77,0x88};

// =====================
// 4. Hardware Pins
// =====================
// Status LEDs
constexpr int RED_LED_PIN   = 33;
constexpr int GREEN_LED_PIN = 26;
// Buttons
constexpr int CONFIG_BUTTON_PIN = 32;
constexpr int RESET_BUTTON_PIN  = 35;
// Seven-segment (TM1637) display – optional
constexpr int SEVSEG_DATA_PIN = 23; // DIO
constexpr int SEVSEG_CLK_PIN  = 22; // CLK
// Compile without display driver by toggling this flag
constexpr bool ENABLE_SEVSEG_DISPLAY = true;

// =====================
// 5. Mesh Bootstrap Peers
// =====================
static constexpr uint8_t DEFAULT_PEERS[][6] = {
  {0xEC,0x64,0xC9,0x5D,0xAC,0x18}, // master
  {0xEC,0x64,0xC9,0x5D,0x22,0x20}  // sample node
};
constexpr int NUM_DEFAULT_PEERS = sizeof(DEFAULT_PEERS)/6/sizeof(uint8_t);

// =====================
// 6. Logging
// =====================
constexpr planetopia::utils::LogLevel DEFAULT_LOG_LEVEL = planetopia::utils::LogLevel::LOG_DEBUG;

} // namespace config
} // namespace planetopia

#endif // PROJECT_CONFIG_H
