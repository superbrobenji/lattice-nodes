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
constexpr bool DEV_MODE = false;
// Node role to assume at boot when DEV_MODE is true
// NOTE: For server communication, master node should be true
constexpr bool DEFAULT_DEV_MASTER = true;

// =====================
// 2. Default Behaviour
// =====================
// Adapter instantiated on first boot or in DEV_MODE
// IMPORTANT: For server communication via USB, MUST be SERIAL_ADAPTER
constexpr planetopia::adapter::adapter_types DEFAULT_ADAPTER = planetopia::adapter::adapter_types::SERIAL_ADAPTER;
// Primary mesh-beacon interval (milliseconds)
constexpr unsigned long MASTER_BEACON_INTERVAL_MS = 3000;
// Stale-master threshold: node clears master route after this many ms without a beacon (3× interval)
constexpr uint32_t STALE_MASTER_THRESHOLD_MS = 9000;

// =====================
// 3. Radio / ESP-NOW
// =====================
// Wi-Fi / ESP-NOW channel – ALL nodes must match
constexpr uint8_t WIFI_CHANNEL = 1;
// Global 16-byte AES key – ALWAYS used for ESP-NOW encryption
inline constexpr uint8_t DEFAULT_MESH_KEY[16] = {
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
// GPIO 35 is input-only (no internal pull resistors) — use GPIO 25 or similar
constexpr int RESET_BUTTON_PIN  = 25;
// Seven-segment (TM1637) display – optional
constexpr int SEVSEG_DATA_PIN = 23; // DIO
constexpr int SEVSEG_CLK_PIN  = 22; // CLK
// Compile without display driver by toggling this flag
constexpr bool ENABLE_SEVSEG_DISPLAY = true;

// =====================
// 5. Mesh Bootstrap Peers
// =====================
inline constexpr uint8_t DEFAULT_PEERS[][6] = {
  {0xEC,0x64,0xC9,0x5D,0xAC,0x18}, // master
  {0xEC,0x64,0xC9,0x5D,0x22,0x20}  // sample node
};
constexpr int NUM_DEFAULT_PEERS = sizeof(DEFAULT_PEERS) / sizeof(DEFAULT_PEERS[0]);

// =====================
// 6. Logging
// =====================
// CRITICAL: For server communication, MUST be LOG_NONE to prevent text output
// Only enable logging (LOG_DEBUG, LOG_INFO, etc.) for development/debugging
constexpr planetopia::utils::LogLevel DEFAULT_LOG_LEVEL = planetopia::utils::LogLevel::LOG_NONE;

// =====================
// 7. Global Limits (Tiger Style)
// =====================
// Maximum allowed routing hops in the mesh network
constexpr uint8_t MAX_HOPS = 10;
// Peer staleness threshold (ms) before being considered offline
constexpr uint32_t STALE_PEER_THRESHOLD_MS = 8000UL;
// Routing timeout used by MessageRouter (ms)
constexpr uint32_t ROUTING_TIMEOUT_MS = 5000UL;
// Future limits (message queue, buffer sizes, etc.) can be centralized here

} // namespace config
} // namespace planetopia

#endif // PROJECT_CONFIG_H
