# Phase 0: ESP-IDF Migration + EEPROM→NVS Rewrite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the lattice-nodes firmware from bare Arduino/.ino to an ESP-IDF (CMake) project with arduino-esp32 as a component, enabling `CONFIG_MBEDTLS_CHACHAPOLY_C=y` so the E2E AEAD links; simultaneously replace EepromManager (EEPROM 512-byte map) with NVS/Preferences, eliminating the 98%-full EEPROM ceiling and the need for a temporary `0x99` partition.

**Architecture:** The project becomes a standard ESP-IDF CMake project under `firmware/` (the current `main/` dir is renamed). Arduino APIs (`String`, `Serial`, `WiFi`, `esp_now`, `EEPROM`→removed, `digitalWrite`, etc.) remain available via `arduino-esp32` pulled as an IDF component. A custom `sdkconfig.defaults` enables ChaCha20-Poly1305, disables BT, and trims unused mbedtls suites. `EepromManager` is rewritten onto `<Preferences.h>` (NVS key/value), same singleton interface, no migration path (clean NVS on reflash — per the no-backcompat global constraint). Host tests continue to build via the existing `tests/CMakeLists.txt` with mocks; CI adds an IDF build+size job.

**Tech Stack:** ESP-IDF v5.5.x, arduino-esp32 ^3.3.10 (via `idf_component.yml`), CMake, nanopb, mbedtls (IDF-bundled), GoogleTest (host tests unchanged)

## Global Constraints

- No backwards compatibility — devices are reflashed; no data migration.
- Latest protocol only (proto v3). Hub drops anything != 3.
- `mesh_message` struct is 242 bytes (`static_assert`).
- Host test suite must stay green (unit + e2e) — the tests are the verification gate until the IDF CI build is added.
- All source changes must compile on host (tests) AND on IDF (CI).
- The `#include` paths `src/...` and `../../project_config.h` are relative to the current `main/` dir — these must be updated when the directory structure changes.
- `mesh.pb.h`/`mesh.pb.c` are hand-edited (no regen toolchain on the nodes side); do not regenerate.

---

### Task 1: Restructure Directory Layout for ESP-IDF

**Files:**
- Rename: `main/` → `firmware/main/` (ESP-IDF convention: `main` component under project root)
- Create: `firmware/CMakeLists.txt` (IDF project root)
- Create: `firmware/main/CMakeLists.txt` (main component)
- Create: `firmware/main/idf_component.yml` (arduino-esp32 dependency)
- Create: `firmware/sdkconfig.defaults` (mbedtls, BT, FreeRTOS config)
- Create: `firmware/partitions.csv` (custom partition table — no EEPROM partition needed)
- Rename: `main/main.ino` → `firmware/main/main.cpp`
- Modify: `tests/CMakeLists.txt` — update all `../main/` paths to `../firmware/main/`
- Modify: `.github/workflows/unit-tests.yml` — no change needed (tests/ path unchanged)
- Modify: `.github/workflows/e2e-tests.yml` — no change needed

**Interfaces:**
- Consumes: nothing (first task)
- Produces: a compilable directory layout that host tests can build against; IDF project skeleton that `idf.py build` will accept (actual IDF build verified in Task 6)

- [ ] **Step 1: Create the ESP-IDF project skeleton**

Create `firmware/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)

# Pull in IDF cmake utilities
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(lattice-firmware)
```

Create `firmware/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRC_DIRS "." "src/mesh" "src/mesh/serialization" "src/mesh/serialization/nanopb"
             "src/adapter" "src/adapter/serial" "src/adapter/pir"
             "src/persistence" "src/logging" "src/error"
             "src/hardware/output" "src/hardware/input"
             "src/network" "src/app" "src/crypto"
    INCLUDE_DIRS "." "src" "src/mesh/serialization/nanopb" "src/mesh/serialization"
                 "lib/lattice-protocol/c"
    REQUIRES arduino esp_wifi esp_now nvs_flash mbedtls
)
```

Create `firmware/main/idf_component.yml`:
```yaml
dependencies:
  espressif/arduino-esp32:
    version: "^3.3.10"
```

Create `firmware/sdkconfig.defaults`:
```ini
# Arduino-as-component
CONFIG_AUTOSTART_ARDUINO=y
CONFIG_FREERTOS_HZ=1000

# mbedtls: enable ChaCha20-Poly1305 (links the E2E AEAD)
CONFIG_MBEDTLS_CHACHA20_C=y
CONFIG_MBEDTLS_POLY1305_C=y
CONFIG_MBEDTLS_CHACHAPOLY_C=y

# mbedtls: trim unused suites (X25519 + chachapoly + SHA-256 is all we need)
CONFIG_MBEDTLS_TLS_ENABLED=n
CONFIG_MBEDTLS_KEY_EXCHANGE_RSA=n
CONFIG_MBEDTLS_KEY_EXCHANGE_DHE_RSA=n
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA=n
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA=n
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA=n
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDH_RSA=n

# Bluetooth: disabled — unused, saves ~10KB flash + 20-30mA
CONFIG_BT_ENABLED=n

# Partition table: custom (no EEPROM partition — NVS replaces it)
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

Create `firmware/partitions.csv`:
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x1F0000,
```

- [ ] **Step 2: Move main/ to firmware/main/**

```bash
# Move the source tree (preserves git history with `git mv`)
git mv main firmware/main
```

- [ ] **Step 3: Rename main.ino to main.cpp**

```bash
git mv firmware/main/main.ino firmware/main/main.cpp
```

- [ ] **Step 4: Add forward declarations to main.cpp**

Arduino `.ino` files get implicit forward declarations. A `.cpp` file does not. Add explicit declarations at the top of `firmware/main/main.cpp`, after the includes:

```cpp
// Forward declarations (required after .ino → .cpp rename)
static inline void validateServerConfiguration();
void dataRecvCallback(const lattice::mesh::mesh_message& message);
void setup();
void loop();
```

- [ ] **Step 5: Update include paths in tests/CMakeLists.txt**

Replace every `../main/` with `../firmware/main/`:

```cmake
# In include_directories:
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src/mesh/serialization/nanopb
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src/mesh/serialization
)

# In FIRMWARE_SOURCES — replace every ../main/ with ../firmware/main/
set(FIRMWARE_SOURCES
  ../firmware/main/src/mesh/serialization/mesh.pb.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_encode.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_decode.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_common.c
  ../firmware/main/src/persistence/EepromManager.cpp
  ../firmware/main/src/adapter/AdapterFactory.cpp
  ../firmware/main/src/adapter/Adapter.cpp
  ../firmware/main/src/adapter/serial/SerialFraming.cpp
  ../firmware/main/src/adapter/serial/SerialAdapter.cpp
  ../firmware/main/src/adapter/pir/PirAdapter.cpp
  ../firmware/main/src/logging/Logger.cpp
  ../firmware/main/src/error/ErrorCore.cpp
  ../firmware/main/src/hardware/output/GpioOutput.cpp
  ../firmware/main/src/hardware/output/Led.cpp
  ../firmware/main/src/hardware/output/SevenSegDisplay.cpp
  ../firmware/main/src/hardware/input/GpioInput.cpp
  ../firmware/main/src/hardware/input/Pir.cpp
  mocks/esp_now_mock.cpp
  mocks/esp_wifi_mock.cpp
  mocks/time_mock.cpp
  mocks/serial_mock.cpp
  mocks/Arduino.cpp
  mocks/EEPROM.cpp
  mocks/WiFi.cpp
  ../firmware/main/src/mesh/PeerRegistry.cpp
  ../firmware/main/src/mesh/Mesh.cpp
  ../firmware/main/src/mesh/Enrollment.cpp
)
```

Also update the e2e target's harness paths (these reference `e2e/harness/` which is under `tests/` — those don't change).

Update the `clang-format` step in `.github/workflows/unit-tests.yml`:
```yaml
      - name: Check formatting
        run: |
          find firmware/main/src \( -name '*.cpp' -o -name '*.h' \) \
            ! -path '*/nanopb/*' \
            ! -name 'mesh.pb.h' \
            ! -name 'mesh.pb.c' | \
            xargs clang-format --style=file --dry-run --Werror
```

And the `cppcheck` step:
```yaml
      - name: Run cppcheck
        run: |
          cppcheck \
            --error-exitcode=1 \
            --suppress=missingIncludeSystem \
            --suppress=unmatchedSuppression \
            --inline-suppr \
            -I firmware/main/src \
            -i firmware/main/src/mesh/serialization/nanopb \
            -i firmware/main/src/mesh/serialization/mesh.pb.c \
            firmware/main/src/ 2>&1
```

- [ ] **Step 6: Fix internal relative include paths**

Many source files use `#include "src/..."` or `#include "../../project_config.h"` relative to the old `main/` dir. Since `firmware/main/` is now the component root and `INCLUDE_DIRS` includes `"."` and `"src"`, these paths are unchanged — they resolve identically. However, the submodule reference `../../lib/lattice-protocol/c/mesh_message.h` used in `Enrollment.h` etc. needs the submodule to still be at `firmware/main/lib/lattice-protocol`. Verify:

```bash
ls firmware/main/lib/lattice-protocol/c/mesh_message.h
```

Also update `.gitmodules` if the submodule path changed (it was `main/lib/lattice-protocol`, now `firmware/main/lib/lattice-protocol`):

```bash
git config -f .gitmodules submodule.main/lib/lattice-protocol.path firmware/main/lib/lattice-protocol
```

- [ ] **Step 7: Build and run host tests**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

Expected: all unit + e2e tests pass. If any fail due to path issues, fix them before proceeding.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: restructure project for ESP-IDF (main/ → firmware/main/, .ino → .cpp)"
```

---

### Task 2: Rewrite EepromManager onto NVS/Preferences

**Files:**
- Modify: `firmware/main/src/persistence/EepromManager.h` — replace EEPROM includes/addresses with Preferences API; same class interface
- Rewrite: `firmware/main/src/persistence/EepromManager.cpp` — NVS key/value implementation
- Modify: `tests/mocks/EEPROM.h` — add a `Preferences` mock (or create `tests/mocks/Preferences.h`)
- Create: `tests/mocks/Preferences.h` — mock Preferences class for host tests
- Create: `tests/mocks/Preferences.cpp` — mock implementation
- Modify: `tests/CMakeLists.txt` — add `mocks/Preferences.cpp` to FIRMWARE_SOURCES, remove `mocks/EEPROM.cpp`
- Modify: `tests/unit/test_eeprom_manager.cpp` — update tests for new NVS-backed interface

**Interfaces:**
- Consumes: Task 1 directory layout
- Produces: `EepromManager` singleton with identical public API but backed by NVS Preferences; Preferences mock for host tests; all existing callers unchanged

- [ ] **Step 1: Create the Preferences mock**

Create `tests/mocks/Preferences.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class Preferences {
public:
  bool begin(const char* name, bool readOnly = false) {
    _namespace = name;
    _open = true;
    return true;
  }
  void end() { _open = false; }

  // Getters (return defaultValue if key not found)
  uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
    auto it = _store.find(_ns(key));
    if (it == _store.end()) return defaultValue;
    return it->second.empty() ? defaultValue : it->second[0];
  }
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    auto it = _store.find(_ns(key));
    if (it == _store.end()) return defaultValue;
    if (it->second.size() < 4) return defaultValue;
    uint32_t v = 0;
    memcpy(&v, it->second.data(), 4);
    return v;
  }
  bool getBool(const char* key, bool defaultValue = false) {
    return getUChar(key, defaultValue ? 1 : 0) != 0;
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    auto it = _store.find(_ns(key));
    if (it == _store.end()) return 0;
    size_t len = std::min(maxLen, it->second.size());
    memcpy(buf, it->second.data(), len);
    return len;
  }
  bool isKey(const char* key) {
    return _store.find(_ns(key)) != _store.end();
  }

  // Setters (return bytes written; 0 on failure)
  size_t putUChar(const char* key, uint8_t value) {
    _store[_ns(key)] = {value};
    return 1;
  }
  size_t putUInt(const char* key, uint32_t value) {
    std::vector<uint8_t> v(4);
    memcpy(v.data(), &value, 4);
    _store[_ns(key)] = v;
    return 4;
  }
  size_t putBool(const char* key, bool value) {
    return putUChar(key, value ? 1 : 0);
  }
  size_t putBytes(const char* key, const void* buf, size_t len) {
    std::vector<uint8_t> v(len);
    memcpy(v.data(), buf, len);
    _store[_ns(key)] = v;
    return len;
  }

  bool remove(const char* key) {
    return _store.erase(_ns(key)) > 0;
  }
  bool clear() {
    // Clear only keys in current namespace
    std::string prefix = _namespace + "/";
    for (auto it = _store.begin(); it != _store.end();) {
      if (it->first.substr(0, prefix.size()) == prefix)
        it = _store.erase(it);
      else
        ++it;
    }
    return true;
  }

  // Test helper: reset all state
  void reset() { _store.clear(); _namespace.clear(); _open = false; }

  // Static store shared across all instances (mirrors NVS flash persistence)
  static std::map<std::string, std::vector<uint8_t>> _store;

private:
  std::string _namespace;
  bool _open{false};
  std::string _ns(const char* key) { return _namespace + "/" + key; }
};
```

Create `tests/mocks/Preferences.cpp`:
```cpp
#include "Preferences.h"
std::map<std::string, std::vector<uint8_t>> Preferences::_store;
```

- [ ] **Step 2: Rewrite EepromManager.h**

Replace the header. Keep the same class name, singleton pattern, and public method signatures. Remove all EEPROM address constants and size constants. Add NVS key name constants instead:

```cpp
#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "src/logging/Logger.h"
#include "../../project_config.h"

namespace lattice {
namespace utils {

// NVS key names — each maps to one persisted value
namespace NVS_KEYS {
constexpr const char* NAMESPACE        = "lattice";
constexpr const char* MASTER_FLAG      = "master";
constexpr const char* DEV_FLAG         = "dev";
constexpr const char* ADAPTER_TYPE     = "adapter";
constexpr const char* MESH_KEY         = "meshkey";
constexpr const char* PEER_LIST        = "peers";
constexpr const char* REBOOT_REASON    = "rbt_reason";
constexpr const char* REBOOT_COUNT     = "rbt_count";
constexpr const char* PRIVATE_KEY      = "privkey";
constexpr const char* PUBLIC_KEY       = "pubkey";
constexpr const char* KEYPAIR_CRC      = "kp_crc";
constexpr const char* ENROLLED_FLAG    = "enrolled";
constexpr const char* BOOT_EPOCH       = "epoch";
constexpr const char* KNOWN_MASTER_MAC = "master_mac";
constexpr const char* KNOWN_MASTER_MAC_SEC = "master_mac2";
constexpr const char* TX_POWER_PRESET  = "txpower";
constexpr const char* NODE_ID          = "node_id";
} // namespace NVS_KEYS

// Size constants (unchanged from EEPROM era)
namespace EEPROM_SIZES {
constexpr uint8_t MESH_KEY_SIZE = 16;
constexpr uint8_t MAX_PEERS = 10;
constexpr uint8_t PEER_MAC_SIZE = 6;
constexpr uint8_t PEER_PUBLIC_KEY_SIZE = 32;
constexpr uint8_t PEER_RECORD_SIZE = PEER_MAC_SIZE + PEER_PUBLIC_KEY_SIZE; // 38 bytes
constexpr uint16_t PEER_LIST_SIZE = MAX_PEERS * PEER_RECORD_SIZE;          // 380 bytes
} // namespace EEPROM_SIZES

class EepromManager {
private:
  bool isInitialized;
  bool isDevMode;
  Preferences _prefs;

  EepromManager();
  bool ensureInitialized();
  void logOperation(const char* operation, const char* details = nullptr);

public:
  static EepromManager& getInstance();

  EepromManager(const EepromManager&) = delete;
  EepromManager& operator=(const EepromManager&) = delete;
  EepromManager(EepromManager&&) = delete;
  EepromManager& operator=(EepromManager&&) = delete;

  bool init();
  void setDevMode(bool devMode);
  bool getDevMode() const;

  bool loadMasterFlag();
  void saveMasterFlag(bool isMaster);

  bool loadDevFlag();
  void saveDevFlag(bool isDev);

  bool loadMeshKey(uint8_t* key, size_t keySize);
  void saveMeshKey(const uint8_t* key, size_t keySize);

  bool loadPeerList(uint8_t* peerRecords, size_t maxPeers);
  void savePeerList(const uint8_t* peerRecords, size_t numPeers);
  bool hasPeers();
  void clearPeerList();

  uint8_t loadAdapterType();
  void saveAdapterType(uint8_t adapterType);

  uint8_t loadRebootCount();
  void saveRebootCount(uint8_t count);
  void saveRebootReason(uint8_t reason);
  uint8_t loadRebootReason();

  bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32);
  void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32);
  bool loadEnrolledFlag();
  void saveEnrolledFlag(bool enrolled);

  uint32_t loadBootEpoch();
  void saveBootEpoch(uint32_t epoch);

  bool loadKnownMasterMac(uint8_t* mac);
  void saveKnownMasterMac(const uint8_t* mac);
  void clearKnownMasterMac();

  bool loadKnownMasterMacSecondary(uint8_t* mac);
  void saveKnownMasterMacSecondary(const uint8_t* mac);
  void clearKnownMasterMacSecondary();

  lattice::config::TxPowerPreset loadTxPowerPreset();
  void saveTxPowerPreset(lattice::config::TxPowerPreset preset);

  uint8_t loadNodeId();
  void saveNodeId(uint8_t nodeId);

  // flushIfDirty/forceFlush become no-ops (NVS commits per write)
  void flushIfDirty() {}
  void forceFlush() {}

  void clearAll();
  void dumpEEPROM(); // retained for debug compatibility

  ~EepromManager();

#ifdef UNIT_TEST
  bool isInitializedForTest() const { return isInitialized; }
#endif
};

} // namespace utils
} // namespace lattice

#endif // EEPROM_MANAGER_H
```

- [ ] **Step 3: Rewrite EepromManager.cpp**

Full NVS implementation. Each method opens Preferences in the shared namespace, does one read/write, returns. No address arithmetic, no migration, no schema versioning. Key examples:

```cpp
#include "EepromManager.h"
#include "src/error/Error.h"

namespace lattice {
namespace utils {

EepromManager::EepromManager() : isInitialized(false), isDevMode(false) {}

EepromManager::~EepromManager() {
  if (isInitialized) {
    _prefs.end();
  }
}

EepromManager& EepromManager::getInstance() {
  static EepromManager instance;
  return instance;
}

bool EepromManager::init() {
  if (isInitialized) return true;
  if (!_prefs.begin(NVS_KEYS::NAMESPACE, false)) {
    Logger::logln("NVS", "Failed to open NVS namespace", LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromManager: NVS begin failed");
    return false;
  }
  isInitialized = true;

  // Validate WDT tracking bytes (same as before)
  uint8_t reason = _prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
  if (reason == 0x00) {
    _prefs.putUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
  }
  uint8_t count = _prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
  if (count > 10) {
    _prefs.putUChar(NVS_KEYS::REBOOT_COUNT, 0);
  }

  logOperation("Initialized", "NVS ready");
  return true;
}

void EepromManager::setDevMode(bool devMode) { isDevMode = devMode; }
bool EepromManager::getDevMode() const { return isDevMode; }

bool EepromManager::ensureInitialized() {
  if (!isInitialized) {
    Logger::logln("NVS", "NVS not initialized", LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void EepromManager::logOperation(const char* operation, const char* details) {
  if (details) {
    Logger::logln("NVS", String(operation) + ": " + details, LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("NVS", operation, LogLevel::LOG_DEBUG);
  }
}

// --- Master flag ---
bool EepromManager::loadMasterFlag() {
  if (!ensureInitialized()) return false;
  return _prefs.getBool(NVS_KEYS::MASTER_FLAG, false);
}
void EepromManager::saveMasterFlag(bool isMaster) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBool(NVS_KEYS::MASTER_FLAG, isMaster);
}

// --- Dev flag ---
bool EepromManager::loadDevFlag() {
  if (!ensureInitialized()) return false;
  return _prefs.getBool(NVS_KEYS::DEV_FLAG, false);
}
void EepromManager::saveDevFlag(bool isDev) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBool(NVS_KEYS::DEV_FLAG, isDev);
}

// --- Mesh key ---
bool EepromManager::loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized()) return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) return false;
  size_t read = _prefs.getBytes(NVS_KEYS::MESH_KEY, key, keySize);
  return read == keySize;
}
void EepromManager::saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!ensureInitialized() || isDevMode) return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) return;
  _prefs.putBytes(NVS_KEYS::MESH_KEY, key, keySize);
}

// --- Peer list (stored as a single blob: numPeers * PEER_RECORD_SIZE bytes) ---
bool EepromManager::loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  if (!ensureInitialized()) return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS) return false;
  size_t maxBytes = maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  size_t read = _prefs.getBytes(NVS_KEYS::PEER_LIST, peerRecords, maxBytes);
  if (read == 0) {
    memset(peerRecords, 0xFF, maxBytes);
    return false;
  }
  return true;
}
void EepromManager::savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  if (!ensureInitialized() || isDevMode) return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS) return;
  _prefs.putBytes(NVS_KEYS::PEER_LIST, peerRecords,
                  numPeers * EEPROM_SIZES::PEER_RECORD_SIZE);
}
bool EepromManager::hasPeers() {
  if (!ensureInitialized()) return false;
  return _prefs.isKey(NVS_KEYS::PEER_LIST);
}
void EepromManager::clearPeerList() {
  if (!ensureInitialized()) return;
  _prefs.remove(NVS_KEYS::PEER_LIST);
}

// --- Adapter type ---
uint8_t EepromManager::loadAdapterType() {
  if (!ensureInitialized()) return 0;
  return _prefs.getUChar(NVS_KEYS::ADAPTER_TYPE, 0);
}
void EepromManager::saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putUChar(NVS_KEYS::ADAPTER_TYPE, adapterType);
}

// --- Reboot tracking ---
uint8_t EepromManager::loadRebootCount() {
  if (!ensureInitialized()) return 0;
  return _prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
}
void EepromManager::saveRebootCount(uint8_t count) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putUChar(NVS_KEYS::REBOOT_COUNT, count);
}
void EepromManager::saveRebootReason(uint8_t reason) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putUChar(NVS_KEYS::REBOOT_REASON, reason);
}
uint8_t EepromManager::loadRebootReason() {
  if (!ensureInitialized()) return 0xFF;
  return _prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
}

// CRC16 (CCITT)
static uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// --- Keypair ---
bool EepromManager::loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!ensureInitialized()) return false;
  size_t privRead = _prefs.getBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubRead = _prefs.getBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  if (privRead != 32 || pubRead != 32) return false;
  uint16_t stored = static_cast<uint16_t>(_prefs.getUInt(NVS_KEYS::KEYPAIR_CRC, 0xFFFF));
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = crc16(both, 64);
  if (stored != computed) {
    Logger::logln("NVS", "Keypair CRC mismatch", LogLevel::LOG_WARN);
    return false;
  }
  return true;
}
void EepromManager::saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  _prefs.putBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = crc16(both, 64);
  _prefs.putUInt(NVS_KEYS::KEYPAIR_CRC, crc);
}

// --- Enrolled flag ---
bool EepromManager::loadEnrolledFlag() {
  if (!ensureInitialized()) return false;
  return _prefs.getBool(NVS_KEYS::ENROLLED_FLAG, false);
}
void EepromManager::saveEnrolledFlag(bool enrolled) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBool(NVS_KEYS::ENROLLED_FLAG, enrolled);
}

// --- Boot epoch ---
uint32_t EepromManager::loadBootEpoch() {
  if (!ensureInitialized()) return 0;
  return _prefs.getUInt(NVS_KEYS::BOOT_EPOCH, 0);
}
void EepromManager::saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch);
}

// --- TOFU master MAC ---
bool EepromManager::loadKnownMasterMac(uint8_t* mac) {
  if (!ensureInitialized()) return false;
  size_t read = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  if (read != 6) return false;
  bool allFF = true;
  for (int i = 0; i < 6; ++i) if (mac[i] != 0xFF) { allFF = false; break; }
  return !allFF;
}
void EepromManager::saveKnownMasterMac(const uint8_t* mac) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
}
void EepromManager::clearKnownMasterMac() {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC);
}

// --- TOFU secondary master MAC ---
bool EepromManager::loadKnownMasterMacSecondary(uint8_t* mac) {
  if (!ensureInitialized()) return false;
  size_t read = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  if (read != 6) return false;
  bool allFF = true;
  for (int i = 0; i < 6; ++i) if (mac[i] != 0xFF) { allFF = false; break; }
  return !allFF;
}
void EepromManager::saveKnownMasterMacSecondary(const uint8_t* mac) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
}
void EepromManager::clearKnownMasterMacSecondary() {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC_SEC);
}

// --- TX power preset ---
lattice::config::TxPowerPreset EepromManager::loadTxPowerPreset() {
  if (!ensureInitialized()) return lattice::config::DEFAULT_TX_POWER_PRESET;
  uint8_t val = _prefs.getUChar(NVS_KEYS::TX_POWER_PRESET, 0xFF);
  if (val > 2) return lattice::config::DEFAULT_TX_POWER_PRESET;
  return static_cast<lattice::config::TxPowerPreset>(val);
}
void EepromManager::saveTxPowerPreset(lattice::config::TxPowerPreset preset) {
  if (!ensureInitialized() || isDevMode) return;
  _prefs.putUChar(NVS_KEYS::TX_POWER_PRESET, static_cast<uint8_t>(preset));
}

// --- Node ID ---
uint8_t EepromManager::loadNodeId() {
  if (!ensureInitialized()) return 0;
  uint8_t raw = _prefs.getUChar(NVS_KEYS::NODE_ID, 0);
  return raw;
}
void EepromManager::saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized()) return;
  _prefs.putUChar(NVS_KEYS::NODE_ID, nodeId);
}

// --- Utility ---
void EepromManager::clearAll() {
  if (!ensureInitialized()) return;
  _prefs.clear();
  logOperation("All NVS cleared");
}

void EepromManager::dumpEEPROM() {
  Logger::logln("NVS", "NVS dump not implemented (use idf.py nvs-dump)", LogLevel::LOG_INFO);
}

} // namespace utils
} // namespace lattice
```

- [ ] **Step 4: Update the Preferences mock's include in EEPROM.h mock**

The existing `tests/mocks/EEPROM.h` mock is still needed by `mocks/EEPROM.cpp` and may be referenced by tests. Keep it, but it's no longer included by EepromManager. Instead, ensure `Preferences.h` is in the mock include path (it already is — `mocks/` is first in the include path).

Update `tests/CMakeLists.txt` to add the Preferences mock and remove `mocks/EEPROM.cpp` from FIRMWARE_SOURCES:

Replace `mocks/EEPROM.cpp` with `mocks/Preferences.cpp` in FIRMWARE_SOURCES. Keep `mocks/EEPROM.cpp` only if other test files still `#include <EEPROM.h>` — check and keep if needed (the mock is harmless).

Actually, keep both: `EEPROM.h` mock is still needed by some test files that include headers which transitively include it. Add `mocks/Preferences.cpp` alongside:

```cmake
  mocks/EEPROM.cpp
  mocks/Preferences.cpp
```

- [ ] **Step 5: Remove EEPROM include from EepromManager.h**

The new header includes `<Preferences.h>` instead of `<EEPROM.h>`. Remove the `#include <EEPROM.h>` line. Also remove the `EEPROM_ADDRESSES` namespace entirely and the old migration-related V1 addresses.

- [ ] **Step 6: Remove clearRange, isAddressValid, printAddress methods**

These are EEPROM-address-specific utilities. Remove from both `.h` and `.cpp`. If any caller uses them, update or remove the call (grep for `clearRange`, `isAddressValid`, `printAddress`).

- [ ] **Step 7: Update test_eeprom_manager.cpp**

The existing tests reference EEPROM_ADDRESSES and EEPROM-specific behavior (address arithmetic, schema migration). Rewrite tests to verify the NVS-backed interface:

Key tests to keep/update:
- `Init_Succeeds` — `init()` returns true
- `SaveAndLoadMasterFlag` — round-trip bool
- `SaveAndLoadMeshKey` — round-trip 16-byte blob
- `SaveAndLoadKeypair` — round-trip 64 bytes + CRC
- `SaveAndLoadPeerList` — round-trip records
- `SaveAndLoadBootEpoch` — round-trip uint32
- `SaveAndLoadKnownMasterMac` — round-trip 6 bytes, unset returns false
- `DevModeSkipsWrites` — set devMode, verify writes are no-ops
- `ClearAll` — clears all keys

Remove:
- Schema migration tests (v1→v2, v2→v3)
- Address range tests

- [ ] **Step 8: Build and run all tests**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat: rewrite EepromManager onto NVS/Preferences (closes #50)

Replace the 512-byte EEPROM fixed-map persistence with ESP-IDF NVS
key/value storage via the Arduino Preferences library. No migration
path — devices are reflashed (no-backcompat constraint).

Eliminates the 98%-full EEPROM ceiling, manual address arithmetic,
and schema version gating. Same singleton interface; all callers
unchanged."
```

---

### Task 3: Remove EEPROM.h include from main.cpp and remaining files

**Files:**
- Modify: `firmware/main/main.cpp` — remove `#include <EEPROM.h>` if present (it shouldn't be — EepromManager encapsulates it — but verify)
- Grep: confirm no source file under `firmware/main/src/` includes `<EEPROM.h>` except the old EepromManager (now removed)
- Modify: any file still including `<EEPROM.h>` — switch to `EepromManager.h`

**Interfaces:**
- Consumes: Task 2 (EepromManager is now NVS-backed)
- Produces: zero `<EEPROM.h>` includes in the firmware source tree

- [ ] **Step 1: Grep for stale EEPROM includes**

```bash
grep -rn '#include.*EEPROM' firmware/main/src/ firmware/main/main.cpp
```

Expected: no results (EepromManager.h no longer includes EEPROM.h).

- [ ] **Step 2: Fix any findings**

If any file still includes `<EEPROM.h>`, replace with the appropriate `EepromManager.h` include.

- [ ] **Step 3: Remove btStop() from main.cpp**

With `CONFIG_BT_ENABLED=n` in sdkconfig, `btStop()` is unnecessary (BT stack isn't compiled). The call may fail to compile under IDF without BT. Remove it from `setup()`:

```cpp
// Remove this line from setup():
// btStop();
```

- [ ] **Step 4: Build and run tests**

```bash
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: remove stale EEPROM includes and btStop() call"
```

---

### Task 4: Add #ifndef guard to mesh_message.h (lattice-protocol upstream)

**Files:**
- Modify: `firmware/main/lib/lattice-protocol/c/mesh_message.h` — add traditional `#ifndef` guard wrapping the `#pragma once`

**Interfaces:**
- Consumes: nothing
- Produces: `mesh_message.h` protected against duplicate inclusion via two different include-path spellings (compile blocker #3 from umbrella spec)

Note: this is a local submodule fix. The proper upstream fix is in `cmd/gen-headers` in lattice-protocol — but that's Phase E scope. For now, patch the local copy.

- [ ] **Step 1: Add include guard**

```cpp
#ifndef MESH_MESSAGE_H
#define MESH_MESSAGE_H

// Code generated by cmd/gen-headers; DO NOT EDIT.
// ... (existing content) ...

#endif // MESH_MESSAGE_H
```

The file already has `#pragma once`, but the duplicate-include bug happens when the same file is found via two different `-I` paths — `#pragma once` deduplicates by file identity (inode), which fails across paths on some toolchains. The `#ifndef` guard is content-based and always works.

- [ ] **Step 2: Build tests**

```bash
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "fix: add #ifndef guard to mesh_message.h (compile blocker #3)"
```

---

### Task 5: Fix ButtonHandler.h non-self-contained header (compile blocker #2)

**Files:**
- Modify: `firmware/main/src/app/ButtonHandler.h` — fully-qualify `Logger`/`LogLevel` references

**Interfaces:**
- Consumes: nothing
- Produces: ButtonHandler.h compiles without requiring prior `using namespace` declarations

- [ ] **Step 1: Check current ButtonHandler.h**

```bash
grep -n 'Logger\|LogLevel' firmware/main/src/app/ButtonHandler.h
```

- [ ] **Step 2: Fully qualify all references**

Replace unqualified `Logger` with `lattice::utils::Logger` and `LogLevel` with `lattice::utils::LogLevel` throughout the file. Add the necessary `#include "src/logging/Logger.h"` if not already present.

- [ ] **Step 3: Build tests**

```bash
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "fix: fully-qualify Logger/LogLevel in ButtonHandler.h (compile blocker #2)"
```

---

### Task 6: Add CI ESP-IDF Build + Size Report

**Files:**
- Create: `.github/workflows/firmware-build.yml` — ESP-IDF build + size report job
- Modify: `.github/workflows/unit-tests.yml` — update `clang-format` and `cppcheck` paths (if not done in Task 1)

**Interfaces:**
- Consumes: Task 1 (project layout), Task 2-5 (all compile fixes)
- Produces: CI workflow that builds the firmware with ESP-IDF and reports binary size

- [ ] **Step 1: Create firmware build workflow**

Create `.github/workflows/firmware-build.yml`:
```yaml
name: Firmware Build

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Build with ESP-IDF
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5.1
          target: esp32
          path: firmware

      - name: Size report
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5.1
          target: esp32
          path: firmware
          command: idf.py size

      - name: Upload size report
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: firmware-size-report
          path: firmware/build/size_info/
```

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/firmware-build.yml
git commit -m "ci: add ESP-IDF firmware build + size report workflow (closes #49, #54)"
```

---

### Task 7: Integration Verification

**Files:**
- No new files

**Interfaces:**
- Consumes: all previous tasks
- Produces: verified green test suite, updated memory_usage.md (if existing)

- [ ] **Step 1: Full clean rebuild and test**

```bash
rm -rf tests/build
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4
```

Expected: ALL tests pass (unit + e2e).

- [ ] **Step 2: Verify no stale references**

```bash
# No references to old main/ path (except in docs/git history)
grep -rn '"../main/' tests/ firmware/ .github/ --include='*.cmake' --include='*.yml' --include='*.txt'
# No EEPROM.h includes in firmware source
grep -rn '#include.*<EEPROM.h>' firmware/main/src/
# No EEPROM_ADDRESSES references in firmware source
grep -rn 'EEPROM_ADDRESSES' firmware/main/src/
```

All should return empty.

- [ ] **Step 3: Verify submodule path**

```bash
git submodule status
# Should show firmware/main/lib/lattice-protocol at the correct commit
```

- [ ] **Step 4: Commit any fixups**

```bash
git add -A
git diff --cached --stat
# If there are changes:
git commit -m "fix: integration fixups for ESP-IDF migration"
```
