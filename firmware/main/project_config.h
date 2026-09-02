#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include <cstdint>
#include "src/logging/LogLevelConfig.h"
#include "src/logging/Logger.h"
#include "src/adapter/Adapter.h"

namespace lattice {
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
constexpr lattice::adapter::adapter_types DEFAULT_ADAPTER =
    lattice::adapter::adapter_types::SERIAL_ADAPTER;
// Primary mesh-beacon interval (milliseconds)
constexpr unsigned long MASTER_BEACON_INTERVAL_MS = 3000;
// Stale-master threshold: node clears master route after this many ms without a beacon (3×
// interval)
constexpr uint32_t STALE_MASTER_THRESHOLD_MS = 9000;
// Enable for deployments with two physically separate master nodes.
// When false (production default), standard single-master TOFU enforcement applies.
constexpr bool DUAL_MASTER_MODE = false;
// Per-node relay jitter window (ms) — non-master nodes delay relay by [10, 10+RELAY_JITTER_MAX_MS)
// ms to stagger transmissions and prevent collision bursts when all nodes relay simultaneously
constexpr uint8_t RELAY_JITTER_MAX_MS = 64;

// =====================
// 3. Radio / ESP-NOW
// =====================
// Wi-Fi / ESP-NOW channel – ALL nodes must match
constexpr uint8_t WIFI_CHANNEL = 1;
// Global 16-byte AES key – ALWAYS used for ESP-NOW encryption.
// WARNING: Change this before deployment. Every node in a mesh must share the same key.
// Generate a random key: python3 -c "import os; print([hex(b) for b in os.urandom(16)])"
inline constexpr uint8_t DEFAULT_MESH_KEY[16] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
                                                 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

// =====================
// 4. Hardware Pins
// =====================
// Status LEDs
constexpr int RED_LED_PIN = 33;
constexpr int GREEN_LED_PIN = 26;
// Buttons
constexpr int CONFIG_BUTTON_PIN = 32;
// GPIO 35 is input-only (no internal pull resistors) — use GPIO 25 or similar
constexpr int RESET_BUTTON_PIN = 25;
// Seven-segment (TM1637) display – optional
constexpr int SEVSEG_DATA_PIN = 23; // DIO
constexpr int SEVSEG_CLK_PIN = 22;  // CLK
// Compile without display driver by toggling this flag
constexpr bool ENABLE_SEVSEG_DISPLAY = true;

// =====================
// 5. Mesh Bootstrap Peers
// =====================
// TODO: Replace these with your actual device MAC addresses before flashing.
// Run `esptool.py chip_id` or read from the serial output on first boot.
// All nodes in a mesh must share identical WIFI_CHANNEL and DEFAULT_MESH_KEY.
inline constexpr uint8_t DEFAULT_PEERS[][6] = {
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}, // master — replace with real MAC
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02}  // node   — replace with real MAC
};
constexpr int NUM_DEFAULT_PEERS = sizeof(DEFAULT_PEERS) / sizeof(DEFAULT_PEERS[0]);

// =====================
// 6. Logging
// =====================
// CRITICAL: For server communication, MUST be LOG_NONE to prevent text output
// Only enable logging (LOG_DEBUG, LOG_INFO, etc.) for development/debugging.
//
// Not set here: DEFAULT_LOG_LEVEL is derived from the LATTICE_DEFAULT_LOG_LEVEL macro in
// src/logging/LogLevelConfig.h, the single place the level lives. Logger.h's compile-time
// LATTICE_LOG/LATTICE_LOGLN/LATTICE_LOGF gating keys off the same macro, so the runtime value and
// the compile-time gate can never disagree, whichever header a translation unit includes first
// (issue #117). To raise the level for bench debugging, edit the default in LogLevelConfig.h or
// build with `idf.py -DLATTICE_DEFAULT_LOG_LEVEL=<0..4> build` (0=DEBUG .. 3=ERROR, 4=NONE).
constexpr lattice::utils::LogLevel DEFAULT_LOG_LEVEL =
    static_cast<lattice::utils::LogLevel>(LATTICE_DEFAULT_LOG_LEVEL);
static_assert(LATTICE_DEFAULT_LOG_LEVEL >= static_cast<int>(lattice::utils::LogLevel::LOG_DEBUG) &&
                  LATTICE_DEFAULT_LOG_LEVEL <= static_cast<int>(lattice::utils::LogLevel::LOG_NONE),
              "LATTICE_DEFAULT_LOG_LEVEL must be 0 (LOG_DEBUG) .. 4 (LOG_NONE) — see "
              "src/logging/LogLevelConfig.h.");

// =====================
// 7. TX Power Presets
// =====================
// Named presets — admin-friendly, no RF knowledge needed.
// Stored in EEPROM and applied on boot.
enum class TxPowerPreset : uint8_t {
  SHORT_RANGE = 0, // 2dBm  — same room
  INDOOR = 1,      // 14dBm — through walls, building-wide
  OUTDOOR = 2,     // 20dBm — outdoor, maximum range (default)
};

// Maps preset → esp_wifi_set_max_tx_power() value (units of 0.25dBm)
static constexpr uint8_t TX_POWER_VALUES[] = {8, 56, 80};

constexpr TxPowerPreset DEFAULT_TX_POWER_PRESET = TxPowerPreset::OUTDOOR;

// =====================
// 8. Simulation Mode
// =====================
// Set to 1 (or define via -DSIMULATE_MODE=1 build flag) to enable simulation mode
// (serial-injected fake events for single-device dev/test). Never enabled in production.
#ifndef SIMULATE_MODE
#define SIMULATE_MODE 0
#endif

// =====================
// 9. Global Limits (Tiger Style)
// =====================
// E2E AEAD derived-key cache entries (spec §2). One entry per (peer, master) pair;
// masters need one per enrolled node, leaves need one per master. Default: MAX_PEERS.
// (Phase G audit item B) Role-split: this is the MASTER-side cap — masters need one
// slot per enrolled node. E2EKeyStore allocates to this size by default (so
// standalone/unit-test construction keeps today's behaviour) and is shrunk to
// LATTICE_E2E_KEYCACHE_MAX_LEAF via E2EKeyStore::setCapacity() once a node's role is
// known to be a leaf (Mesh::reevaluateRouteTable — mirrors the RouteTable role-split
// from Phase B). ~576 B RAM saved per leaf.
inline constexpr size_t LATTICE_E2E_KEYCACHE_MAX = 10; // = MAX_PEERS
// Leaf-side E2E keycache cap (Phase G audit item B): a leaf only ever derives keys
// for its primary and (if dual-master) secondary master — never for other leaves.
inline constexpr size_t LATTICE_E2E_KEYCACHE_MAX_LEAF = 2;
// Multi-hop uplink routing (spec §3): max beacon-learned forwarding neighbors
// tracked per node. One entry per distinct upstream relay a node can hear.
inline constexpr size_t LATTICE_NEIGHBOR_MAX = 8;
// Per-origin ReplayCache slot count (issue #46). Bounds memory to
// LATTICE_REPLAY_MAX_ORIGINS × sizeof(ReplayCache::Entry). Size to
// (expected concurrent origins × 1.5). Default matches the old ring size.
// (Phase G §6) Trimmed 16 -> 12: per-origin high-water observed in Phase A/B
// testing never exceeded 8 concurrent origins; 12 keeps a 1.5x margin.
constexpr size_t LATTICE_REPLAY_MAX_ORIGINS = 12;
// Downlink source routing (spec §4): max node->path entries the master tracks.
// Master is hub-side with RAM headroom; raise for large deployments.
// (Phase G §5) Trimmed 32 -> 16 (master-only allocation, Phase B): sufficient for
// realistic deployment fan-out; raise if a master ever accumulates more nodes.
inline constexpr size_t LATTICE_ROUTE_TABLE_MAX = 16;
// Downlink auto-registered forwarding peers (spec §2: "20-peer cap,
// LRU-evicted"). Bounds the number of non-enrolled, non-master ESP-NOW peers a
// node will auto-register while relaying/sending source-routed downlink
// frames (DownlinkRouter::registerDownlinkPeer, DownlinkRouter.cpp) — without this bound, an
// RF attacker can craft ADAPTER_DATA frames with fresh distinct next-hop MACs
// to exhaust the ~20-slot ESP-NOW peer table (no self-heal, no reboot),
// blackholing legitimate downlink forwarding. Keep small: enrolled peers
// (up to MAX_PEERS) + the single uplink forwardingPeer + this must stay well
// under the ~20 cap on realistic relays.
inline constexpr size_t LATTICE_DOWNLINK_PEER_MAX = 4;
// Maximum allowed routing hops in the mesh network
// (protocol v0.6.0 wire shrink §8: routePath 60→48B, 10→8 hops. Observed
// deployments never exceeded 4 hops, so this is safe with margin.)
constexpr uint8_t MAX_HOPS = 8;
// Peer staleness threshold (ms) before being considered offline
constexpr uint32_t STALE_PEER_THRESHOLD_MS = 8000UL;
// Routing timeout used by MessageRouter (ms)
constexpr uint32_t ROUTING_TIMEOUT_MS = 5000UL;
// Health report interval (ms) — periodic send every 30 seconds
constexpr uint32_t HEALTH_REPORT_INTERVAL_MS = 30000;
// Route report interval — 2× health report interval (60 seconds)
constexpr uint32_t ROUTE_REPORT_INTERVAL_MS = HEALTH_REPORT_INTERVAL_MS * 2;
// Future limits (message queue, buffer sizes, etc.) can be centralized here

} // namespace config
} // namespace lattice

#endif // PROJECT_CONFIG_H
