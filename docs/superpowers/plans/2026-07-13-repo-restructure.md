# Repo Restructure & Code Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full directory restructure, file decomposition, and naming normalisation of lattice-nodes firmware — zero functional change.

**Architecture:** Work bottom-up: rename leaf dependencies first (Logger → `logging/`, `EepromManager`), then rename mid-tier dirs and classes (`mesh/`, `adapter/`, `PirAdapter`, `SerialAdapter`), then extract Mesh sub-components (`ReplayCache`, `MeshCrypto`, `PeerRegistry`, `Enrollment`), then extract `SerialFraming`, then create `app/` layer from `main.ino`. Tests pass after every task. A key structural improvement: once mbedtls is isolated in `MeshCrypto.h` / `Enrollment.cpp`, `mesh_logic_impl.cpp` shrinks — the production `.cpp` files are compiled directly in tests.

**Tech Stack:** C++17, Arduino/ESP-IDF (ESP32), GoogleTest, CMake (test build), `git mv` for history-preserving renames.

## Global Constraints

- Zero functional change — all logic moves verbatim, no behaviour modifications
- Use `git mv` for all file/directory renames (preserves history)
- Tests must pass after every task: `cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- No heap allocation introduced; static arrays only
- `IRAM_ATTR` callbacks must remain in `Mesh.cpp`
- `project_config.h` stays at `main/project_config.h` (Arduino IDE requirement)
- All paths in this plan are relative to repo root (`lattice-nodes/`)
- Include paths in firmware source are rooted at `main/` (Arduino sketch root) OR `main/src/` (CMake include root — check CMakeLists.txt line 19–26)

---

## File Map

**New files created:**
- `main/src/logging/Logger.h/.cpp` (moved from `src/core/`)
- `main/src/persistence/EepromManager.h/.cpp` (moved + renamed from `EEPROM_Manager`)
- `main/src/mesh/` entire dir (moved from `src/Mesh/`)
- `main/src/mesh/ReplayCache.h` (extracted from Mesh)
- `main/src/mesh/MeshCrypto.h` (extracted from Mesh.cpp)
- `main/src/mesh/PeerRegistry.h/.cpp` (extracted from Mesh)
- `main/src/mesh/Enrollment.h/.cpp` (extracted from Mesh)
- `main/src/adapter/` entire dir (moved from `src/Adapter/`)
- `main/src/adapter/pir/PirAdapter.h/.cpp` (moved + renamed from `PIR_Adapter`)
- `main/src/adapter/serial/SerialAdapter.h/.cpp` (moved + renamed from `Serial_Adapter`)
- `main/src/adapter/serial/SerialFraming.h/.cpp` (extracted from SerialAdapter)
- `main/src/app/BootManager.h` (extracted from `main.ino`)
- `main/src/app/DisplayManager.h` (extracted from `main.ino`)
- `main/src/app/ButtonHandler.h` (extracted from `main.ino`)

**Deleted (via git mv):**
- All original locations of moved files above

**Modified:**
- `main/main.ino` (reduced to ~150 lines)
- `main/project_config.h` (include paths)
- `tests/CMakeLists.txt` (all paths + 3 new sources)
- `tests/mocks/mesh_logic_impl.cpp` (member paths updated; many methods removed — they move to production `.cpp` files compiled in tests)
- `tests/mocks/firmware_stubs.cpp` (add stubs for `Enrollment::init` + `Enrollment::enrollPeer`)
- All test `unit/` files (include paths)
- `REFACTORING_GUIDE.md`

---

### Task 1: Rename `src/core/` → `src/logging/`

**Files:**
- Move: `main/src/core/Logger.h` → `main/src/logging/Logger.h`
- Move: `main/src/core/Logger.cpp` → `main/src/logging/Logger.cpp`
- Modify: `tests/CMakeLists.txt`, `main/project_config.h`, and all firmware files that include Logger

**Interfaces:** Logger API unchanged.

- [ ] **Step 1: Move the directory**

```bash
git mv main/src/core main/src/logging
```

- [ ] **Step 2: Update CMakeLists.txt**

In `tests/CMakeLists.txt`, change:
```cmake
  ../main/src/core/Logger.cpp
```
to:
```cmake
  ../main/src/logging/Logger.cpp
```

- [ ] **Step 3: Update all `#include "src/core/Logger.h"` occurrences**

```bash
grep -rl '"src/core/Logger.h"' main/ tests/
```

Replace in each file found:
```cpp
// OLD:
#include "src/core/Logger.h"
// NEW:
#include "src/logging/Logger.h"
```

Also check for the short form used in mock files:
```bash
grep -rl '"core/Logger.h"' main/ tests/
```
Replace with `"logging/Logger.h"` in any hits.

- [ ] **Step 4: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "refactor: rename src/core/ to src/logging/"
```

---

### Task 2: Rename `EEPROM_Manager` → `EepromManager`

**Files:**
- Move: `main/src/persistence/EEPROM_Manager.h` → `main/src/persistence/EepromManager.h`
- Move: `main/src/persistence/EEPROM_Manager.cpp` → `main/src/persistence/EepromManager.cpp`
- Modify: class internals, all consumers

**Interfaces:** Class renamed `lattice::utils::EepromManager`. Singleton: `EepromManager::getInstance()`. All method signatures unchanged.

- [ ] **Step 1: Move files**

```bash
git mv main/src/persistence/EEPROM_Manager.h main/src/persistence/EepromManager.h
git mv main/src/persistence/EEPROM_Manager.cpp main/src/persistence/EepromManager.cpp
```

- [ ] **Step 2: Rename class declaration in `EepromManager.h`**

```cpp
// OLD:
class EEPROM_Manager {
// ...
  static EEPROM_Manager& getInstance();
  EEPROM_Manager(const EEPROM_Manager&) = delete;
  EEPROM_Manager& operator=(const EEPROM_Manager&) = delete;
  EEPROM_Manager(EEPROM_Manager&&) = delete;
  EEPROM_Manager& operator=(EEPROM_Manager&&) = delete;
// ...
  EEPROM_Manager();

// NEW:
class EepromManager {
// ...
  static EepromManager& getInstance();
  EepromManager(const EepromManager&) = delete;
  EepromManager& operator=(const EepromManager&) = delete;
  EepromManager(EepromManager&&) = delete;
  EepromManager& operator=(EepromManager&&) = delete;
// ...
  EepromManager();
```

Leave the header guard `#ifndef EEPROM_MANAGER_H` unchanged.

- [ ] **Step 3: Rename class in `EepromManager.cpp`**

Replace every `EEPROM_Manager::` → `EepromManager::` and update the singleton:
```cpp
// OLD:
EEPROM_Manager& EEPROM_Manager::getInstance() {
  static EEPROM_Manager instance;
  return instance;
}
// NEW:
EepromManager& EepromManager::getInstance() {
  static EepromManager instance;
  return instance;
}
```

- [ ] **Step 4: Update all consumers**

```bash
grep -rl 'EEPROM_Manager' main/ tests/
```

In every file found, replace:
- `#include "src/persistence/EEPROM_Manager.h"` → `#include "src/persistence/EepromManager.h"`
- `EEPROM_Manager::getInstance()` → `EepromManager::getInstance()`
- `EEPROM_Manager&` → `EepromManager&`

Also check mock/test files for the short include form:
```bash
grep -rl 'EEPROM_Manager' tests/
```

- [ ] **Step 5: Update CMakeLists.txt**

```cmake
# OLD:
  ../main/src/persistence/EEPROM_Manager.cpp
# NEW:
  ../main/src/persistence/EepromManager.cpp
```

- [ ] **Step 6: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "refactor: rename EEPROM_Manager → EepromManager"
```

---

### Task 3: Rename directories + `PIR_Adapter` / `Serial_Adapter` classes

Move `src/Mesh/` → `src/mesh/`, `src/Adapter/` → `src/adapter/`, rename `PIR_Adapter` → `PirAdapter`, `Serial_Adapter` → `SerialAdapter` (class names + files). Do all renames in one task to keep includes consistent.

**Files:**
- Move: `main/src/Mesh/` → `main/src/mesh/`
- Move: `main/src/Adapter/` → `main/src/adapter/`
- Move+rename: `PIR_Adapter/PIR_Adapter.h/.cpp` → `pir/PirAdapter.h/.cpp`
- Move+rename: `Serial_Adapter/Serial_Adapter.h/.cpp` → `serial/SerialAdapter.h/.cpp`
- Modify: all `#include` paths and class references throughout

**Interfaces:**
- `PirAdapter` replaces `PIR_Adapter` — identical API
- `SerialAdapter` replaces `Serial_Adapter` — static method becomes `SerialAdapter::relayEnrollmentToServer`

- [ ] **Step 1: Move Mesh and Adapter directories**

```bash
git mv main/src/Mesh main/src/mesh
git mv main/src/Adapter main/src/adapter
```

- [ ] **Step 2: Rename adapter subdirectories**

```bash
git mv main/src/adapter/PIR_Adapter main/src/adapter/pir
git mv main/src/adapter/Serial_Adapter main/src/adapter/serial
```

- [ ] **Step 3: Rename PIR_Adapter files**

```bash
git mv main/src/adapter/pir/PIR_Adapter.h main/src/adapter/pir/PirAdapter.h
git mv main/src/adapter/pir/PIR_Adapter.cpp main/src/adapter/pir/PirAdapter.cpp
```

- [ ] **Step 4: Rename Serial_Adapter files**

```bash
git mv main/src/adapter/serial/Serial_Adapter.h main/src/adapter/serial/SerialAdapter.h
git mv main/src/adapter/serial/Serial_Adapter.cpp main/src/adapter/serial/SerialAdapter.cpp
```

- [ ] **Step 5: Rename class `PIR_Adapter` → `PirAdapter` in its files**

In `main/src/adapter/pir/PirAdapter.h`:
```cpp
// OLD:
class PIR_Adapter : public Adapter {
public:
  explicit PIR_Adapter(int pirPin);
// NEW:
class PirAdapter : public Adapter {
public:
  explicit PirAdapter(int pirPin);
```

In `main/src/adapter/pir/PirAdapter.cpp`:
```cpp
// Update include:
#include "PirAdapter.h"   // was: #include "PIR_Adapter.h"
// Replace all method definitions:
PirAdapter::PirAdapter(int pirPin) ...   // was: PIR_Adapter::PIR_Adapter
PirAdapter::init() ...                   // was: PIR_Adapter::init
// etc. — replace every PIR_Adapter:: → PirAdapter::
```

- [ ] **Step 6: Rename class `Serial_Adapter` → `SerialAdapter` in its files**

In `main/src/adapter/serial/SerialAdapter.h`:
```cpp
// OLD:
class Serial_Adapter : public Adapter {
public:
  explicit Serial_Adapter(int pin);
  static void relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]);
// NEW:
class SerialAdapter : public Adapter {
public:
  explicit SerialAdapter(int pin);
  static void relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]);
```

In `main/src/adapter/serial/SerialAdapter.cpp`:
```cpp
// Update include:
#include "SerialAdapter.h"   // was: #include "Serial_Adapter.h"
// Replace every Serial_Adapter:: → SerialAdapter::
```

- [ ] **Step 7: Update all `#include` paths and class references**

```bash
grep -rl 'PIR_Adapter\|Serial_Adapter\|src/Mesh\|src/Adapter' main/ tests/
```

Key substitutions across all files found:

```cpp
// Include paths:
"src/Mesh/Mesh.h"                             → "src/mesh/Mesh.h"
"Mesh/Mesh.h"                                 → "mesh/Mesh.h"         // in tests/mocks/
"src/Adapter/Adapter.h"                       → "src/adapter/Adapter.h"
"src/Adapter/AdapterFactory.h"                → "src/adapter/AdapterFactory.h"
"src/Adapter/PIR_Adapter/PIR_Adapter.h"       → "src/adapter/pir/PirAdapter.h"
"src/Adapter/Serial_Adapter/Serial_Adapter.h" → "src/adapter/serial/SerialAdapter.h"
"src/Mesh/serialization/..."                  → "src/mesh/serialization/..."
"src/Mesh/serialization/nanopb/..."           → "src/mesh/serialization/nanopb/..."

// Class references:
PIR_Adapter(         → PirAdapter(
new PIR_Adapter      → new PirAdapter
Serial_Adapter::     → SerialAdapter::
new Serial_Adapter   → new SerialAdapter
```

- [ ] **Step 8: Update CMakeLists.txt — all paths and include dirs**

```cmake
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../main
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/mesh/serialization/nanopb
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/mesh/serialization
)

set(FIRMWARE_SOURCES
  ../main/src/mesh/serialization/mesh.pb.c
  ../main/src/mesh/serialization/nanopb/pb_encode.c
  ../main/src/mesh/serialization/nanopb/pb_decode.c
  ../main/src/mesh/serialization/nanopb/pb_common.c
  ../main/src/persistence/EepromManager.cpp
  ../main/src/adapter/AdapterFactory.cpp
  ../main/src/adapter/Adapter.cpp
  ../main/src/adapter/serial/SerialAdapter.cpp
  ../main/src/adapter/pir/PirAdapter.cpp
  ../main/src/logging/Logger.cpp
  ../main/src/error/ErrorCore.cpp
  ../main/src/hardware/output/GpioOutput.cpp
  ../main/src/hardware/output/Led.cpp
  ../main/src/hardware/output/SevenSegDisplay.cpp
  ../main/src/hardware/input/GpioInput.cpp
  ../main/src/hardware/input/Pir.cpp
  mocks/esp_now_mock.cpp
  mocks/esp_wifi_mock.cpp
  mocks/time_mock.cpp
  mocks/serial_mock.cpp
  mocks/Arduino.cpp
  mocks/EEPROM.cpp
  mocks/WiFi.cpp
  mocks/firmware_stubs.cpp
  mocks/mesh_logic_impl.cpp
)
```

- [ ] **Step 9: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 10: Commit**

```bash
git add -A && git commit -m "refactor: rename mesh/, adapter/ dirs + PirAdapter/SerialAdapter classes"
```

---

### Task 4: Extract `ReplayCache.h`

Extract replay protection into a standalone header-only struct. Mesh composes it as a member. The struct is a pure struct (no Arduino/ESP-NOW deps) — header-only is safe.

**Files:**
- Create: `main/src/mesh/ReplayCache.h`
- Modify: `main/src/mesh/Mesh.h` (remove replay fields, add `ReplayCache replay` member)
- Modify: `main/src/mesh/Mesh.cpp` (use `replay.` prefix for all replay fields)
- Modify: `tests/mocks/mesh_logic_impl.cpp` (use `replay.` prefix)
- Modify: `tests/unit/test_replay_cache.cpp` (test `ReplayCache` directly)

- [ ] **Step 1: Create `main/src/mesh/ReplayCache.h`**

The body of `isReplay` is the exact implementation from `mesh_logic_impl.cpp` (`Mesh::isReplay`, lines 22–35). Move it verbatim, substituting `replayCache` → `cache`, `replayCacheIdx` → `idx`.

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

struct ReplayCache {
  static constexpr size_t CACHE_SIZE = 16;

  struct Entry {
    uint8_t mac[6];
    uint32_t epoch;
    uint16_t seq;
  };

  Entry cache[CACHE_SIZE]{};
  size_t idx{0};
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    idx = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
    memset(cache, 0, sizeof(cache));
  }

  uint16_t nextSeq() { return ++txSeqNum; }

  inline bool isReplay(const mesh_message& msg) {
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
      if (memcmp(cache[i].mac, msg.origin_mac_address, 6) == 0 &&
          cache[i].epoch == msg.epoch_num && cache[i].seq == msg.seq_num) {
        return true;
      }
    }
    memcpy(cache[idx].mac, msg.origin_mac_address, 6);
    cache[idx].epoch = msg.epoch_num;
    cache[idx].seq = msg.seq_num;
    idx = (idx + 1) % CACHE_SIZE;
    return false;
  }
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Update `Mesh.h` — remove replay fields, add composed member**

Add include near the top of `Mesh.h`:
```cpp
#include "ReplayCache.h"
```

Remove from the private section (search for each):
```cpp
// REMOVE:
struct ReplayEntry { uint8_t mac[6]; uint32_t epoch; uint16_t seq; };
static constexpr size_t REPLAY_CACHE_SIZE = 16;
ReplayEntry replayCache[REPLAY_CACHE_SIZE];
size_t replayCacheIdx;
bool isReplay(const mesh_message& msg);
uint32_t bootEpoch;
uint16_t txSeqNum;
uint32_t lastRelayedEpoch;
uint16_t lastRelayedSeqNum;
```

Add to the private section:
```cpp
ReplayCache replay;
```

- [ ] **Step 3: Update `Mesh.cpp` — use `replay.` prefix**

Replace all occurrences (use search-replace):
```
replayCache[  →  replay.cache[
replayCacheIdx  →  replay.idx
REPLAY_CACHE_SIZE  →  ReplayCache::CACHE_SIZE
bootEpoch  →  replay.bootEpoch
txSeqNum  →  replay.txSeqNum
lastRelayedEpoch  →  replay.lastRelayedEpoch
lastRelayedSeqNum  →  replay.lastRelayedSeqNum
isReplay(  →  replay.isReplay(
```

In `Mesh::init()`, find where `bootEpoch` is loaded from EEPROM and replace:
```cpp
// OLD:
bootEpoch = EepromManager::getInstance().loadBootEpoch() + 1;
EepromManager::getInstance().saveBootEpoch(bootEpoch);
txSeqNum = 0;
memset(replayCache, 0, sizeof(replayCache));
replayCacheIdx = 0;
// NEW:
uint32_t epoch = EepromManager::getInstance().loadBootEpoch() + 1;
EepromManager::getInstance().saveBootEpoch(epoch);
replay.init(epoch);
```

- [ ] **Step 4: Update `mesh_logic_impl.cpp` — use `replay.` prefix**

`Mesh::isReplay` is implemented in `mesh_logic_impl.cpp` (lines 22–35). Remove it entirely — it now lives in `ReplayCache::isReplay` inside `ReplayCache.h`, which is compiled everywhere via the header.

In `Mesh::processMasterBeacon` (lines 101–118 of `mesh_logic_impl.cpp`), replace:
```cpp
// OLD:
lastRelayedEpoch = msg.epoch_num;
lastRelayedSeqNum = msg.seq_num;
bool isNewer = (msg.epoch_num > lastRelayedEpoch) ||
               (msg.epoch_num == lastRelayedEpoch && msg.seq_num > lastRelayedSeqNum);
// NEW:
bool isNewer = (msg.epoch_num > replay.lastRelayedEpoch) ||
               (msg.epoch_num == replay.lastRelayedEpoch && msg.seq_num > replay.lastRelayedSeqNum);
// ...
replay.lastRelayedEpoch = msg.epoch_num;
replay.lastRelayedSeqNum = msg.seq_num;
```

In `Mesh::drainRecvQueue` (line 136), the call `isReplay(msg)` becomes `replay.isReplay(msg)`.

- [ ] **Step 5: Update `test_replay_cache.cpp` — test `ReplayCache` directly**

The existing test accesses `Mesh::replayCache` via the `UNIT_TEST` public hack. Replace the entire test to use `ReplayCache` directly — no Mesh needed:

```cpp
#include <gtest/gtest.h>
#include "mesh/ReplayCache.h"

using namespace lattice::mesh;

TEST(ReplayCacheTest, FreshMessageNotReplay) {
  ReplayCache rc;
  rc.init(1);
  mesh_message msg{};
  msg.epoch_num = 1;
  msg.seq_num = 1;
  memset(msg.origin_mac_address, 0xAA, 6);
  EXPECT_FALSE(rc.isReplay(msg));
}

TEST(ReplayCacheTest, DuplicateIsReplay) {
  ReplayCache rc;
  rc.init(1);
  mesh_message msg{};
  msg.epoch_num = 1;
  msg.seq_num = 1;
  memset(msg.origin_mac_address, 0xAA, 6);
  rc.isReplay(msg);   // first — records it
  EXPECT_TRUE(rc.isReplay(msg));  // second — replay
}

TEST(ReplayCacheTest, DifferentSeqNotReplay) {
  ReplayCache rc;
  rc.init(1);
  mesh_message msg{};
  msg.epoch_num = 1;
  msg.seq_num = 1;
  memset(msg.origin_mac_address, 0xAA, 6);
  rc.isReplay(msg);
  msg.seq_num = 2;
  EXPECT_FALSE(rc.isReplay(msg));
}

TEST(ReplayCacheTest, RingBufferWrapsWithoutFalsePositive) {
  ReplayCache rc;
  rc.init(1);
  mesh_message msg{};
  msg.epoch_num = 1;
  memset(msg.origin_mac_address, 0xBB, 6);
  // Fill cache entirely
  for (uint16_t i = 1; i <= ReplayCache::CACHE_SIZE + 1; ++i) {
    msg.seq_num = i;
    EXPECT_FALSE(rc.isReplay(msg));
  }
}
```

Note: keep any existing test cases from the original `test_replay_cache.cpp` that aren't covered above — migrate them to use `ReplayCache` directly.

- [ ] **Step 6: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass, including `test_replay_cache`.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "refactor: extract ReplayCache from Mesh"
```

---

### Task 5: Extract `MeshCrypto.h`

Extract the two static file-scope crypto helpers from `Mesh.cpp` into a header-only namespace. Inline functions are valid C++17 — duplicate definitions across TUs are merged by the linker.

**Files:**
- Create: `main/src/mesh/MeshCrypto.h`
- Modify: `main/src/mesh/Mesh.cpp` (remove static functions, add include, update call sites)

- [ ] **Step 1: Create `main/src/mesh/MeshCrypto.h`**

The function bodies are the verbatim content of `static derivePeerLMK` and `static registerPeerWithEspNow` from `Mesh.cpp` (lines 29–160), plus the keygen branch from `Mesh::loadOrGenerateKeypair`. Change `static void` → `inline void`.

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"

namespace lattice {
namespace mesh {
namespace crypto {

// Move exact body of static derivePeerLMK() from Mesh.cpp (search "static void derivePeerLMK").
// Change: static void → inline void
inline void derivePeerLMK(const uint8_t* ownPriv32, const uint8_t* peerPub32,
                          uint8_t* lmk16Out) {
  // ... verbatim from Mesh.cpp
}

// Move exact body of static registerPeerWithEspNow() from Mesh.cpp.
// Change: static void → inline void
inline void registerPeerWithEspNow(const uint8_t mac[6], const uint8_t* ownPriv32,
                                   const uint8_t* peerPub32) {
  // ... verbatim from Mesh.cpp
  // Internal call: derivePeerLMK(ownPriv32, peerPub32, lmk);  (no change needed)
}

// Extract ONLY the key generation branch from Mesh::loadOrGenerateKeypair().
// The load-from-EEPROM branch stays in Enrollment::init().
inline void generateKeypair(uint8_t* priv32Out, uint8_t* pub32Out) {
  // ... verbatim mbedtls keygen block from Mesh::loadOrGenerateKeypair() generation branch
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Update `Mesh.cpp`**

Add near the top of `Mesh.cpp`:
```cpp
#include "MeshCrypto.h"
```

Remove the two static functions (`derivePeerLMK` and `registerPeerWithEspNow`) entirely from `Mesh.cpp`.

Update all call sites — they previously called these as file-scope statics; now qualify:
```cpp
// OLD:
derivePeerLMK(ownPriv, peerPub, lmk);
registerPeerWithEspNow(mac, devicePrivateKey, peerPubKey);
// NEW:
lattice::mesh::crypto::derivePeerLMK(ownPriv, peerPub, lmk);
lattice::mesh::crypto::registerPeerWithEspNow(mac, devicePrivateKey, peerPubKey);
```

In `Mesh::loadOrGenerateKeypair()`, replace the key generation block with:
```cpp
lattice::mesh::crypto::generateKeypair(devicePrivateKey, devicePublicKey);
```

- [ ] **Step 3: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass. (mbedtls symbols are stubbed in `firmware_stubs.cpp` — no change needed there since `MeshCrypto.h` isn't compiled into test TUs yet; `Mesh.cpp` is still excluded from FIRMWARE_SOURCES.)

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "refactor: extract MeshCrypto.h from Mesh"
```

---

### Task 6: Extract `PeerRegistry`

Extract peer list management into a composable class. `PeerRegistry.cpp` goes into `FIRMWARE_SOURCES` — it has no mbedtls dependency. Methods currently provided by `mesh_logic_impl.cpp` move to the real production file.

**Files:**
- Create: `main/src/mesh/PeerRegistry.h`
- Create: `main/src/mesh/PeerRegistry.cpp`
- Modify: `main/src/mesh/Mesh.h` (remove peer fields + `PeerInfo`/`MasterInfo` structs, add `PeerRegistry peers` member)
- Modify: `main/src/mesh/Mesh.cpp` (use `peers.` prefix; `findNextHopToMaster` stays in Mesh — it uses `currentMaster`)
- Modify: `tests/mocks/mesh_logic_impl.cpp` (remove methods that moved; update remaining)
- Modify: `tests/CMakeLists.txt` (add new source)

**Interfaces:**
- Produces:
```cpp
namespace lattice::mesh {

struct PeerInfo {
  uint8_t mac[6];
  uint8_t publicKey[32];
  uint32_t lastSeenMillis;
};

struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance;
  uint8_t nextHop[6];
};

class PeerRegistry {
public:
  PeerInfo peerMacs[MAX_PEERS]{};   // public for direct iteration in Mesh
  size_t peerCount{0};

  void setDeviceMac(const uint8_t mac[6]);
  PeerInfo* find(const uint8_t mac[6]);
  bool append(const PeerInfo& peer);
  void remove(const uint8_t mac[6]);
  bool isPeerInRange(const uint8_t mac[6]) const;
  void updateLastSeen(const uint8_t mac[6]);
  void loadFromEEPROM();
  void saveToEEPROM();
  void addAndPersist(const uint8_t mac[6]);
  void removeAndPersist(const uint8_t mac[6]);
  size_t count() const { return peerCount; }

private:
  uint8_t deviceMac[6]{};
};
}
```

- [ ] **Step 1: Create `main/src/mesh/PeerRegistry.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "src/persistence/EepromManager.h"
#include "src/network/MacAddress.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

using lattice::utils::EEPROM_SIZES::MAX_PEERS;

struct PeerInfo {
  uint8_t mac[6];
  uint8_t publicKey[32];
  uint32_t lastSeenMillis;
};

struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance;
  uint8_t nextHop[6];
};

class PeerRegistry {
public:
  PeerInfo peerMacs[MAX_PEERS]{};
  size_t peerCount{0};

  PeerRegistry();
  void setDeviceMac(const uint8_t mac[6]);

  PeerInfo* find(const uint8_t mac[6]);
  bool append(const PeerInfo& peer);
  void remove(const uint8_t mac[6]);
  bool isPeerInRange(const uint8_t mac[6]) const;
  void updateLastSeen(const uint8_t mac[6]);

  void loadFromEEPROM();
  void saveToEEPROM();
  void addAndPersist(const uint8_t mac[6]);
  void removeAndPersist(const uint8_t mac[6]);

  size_t count() const { return peerCount; }

private:
  uint8_t deviceMac[6]{};
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Create `main/src/mesh/PeerRegistry.cpp`**

Move the following method bodies **verbatim** from `Mesh.cpp` and `mesh_logic_impl.cpp`, replacing `Mesh::` → `PeerRegistry::`, `peerMacs` → `peerMacs` (same name, now on `this`), `deviceMacAddress` → `deviceMac`:

| Source method | Destination |
|---|---|
| `Mesh::loadPeersFromEEPROM` | `PeerRegistry::loadFromEEPROM` |
| `Mesh::savePeersToEEPROM` | `PeerRegistry::saveToEEPROM` |
| `Mesh::addPeerToEEPROM` | `PeerRegistry::addAndPersist` |
| `Mesh::removePeerFromEEPROM` | `PeerRegistry::removeAndPersist` |
| `Mesh::appendPeer` (mesh_logic_impl.cpp) | `PeerRegistry::append` |
| `Mesh::findPeer` (mesh_logic_impl.cpp) | `PeerRegistry::find` |
| `Mesh::isPeerInRange` (mesh_logic_impl.cpp) | `PeerRegistry::isPeerInRange` |
| `Mesh::updatePeerLastSeen` | `PeerRegistry::updateLastSeen` |

```cpp
#include "PeerRegistry.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"

namespace lattice {
namespace mesh {

PeerRegistry::PeerRegistry() {
  memset(peerMacs, 0, sizeof(peerMacs));
  memset(deviceMac, 0, sizeof(deviceMac));
}

void PeerRegistry::setDeviceMac(const uint8_t mac[6]) {
  memcpy(deviceMac, mac, 6);
}

// ... verbatim bodies from Mesh.cpp / mesh_logic_impl.cpp for each method above
} // namespace mesh
} // namespace lattice
```

`addAndPersist` (was `addPeerToEEPROM`) uses `deviceMacAddress` — replace with `deviceMac`.

- [ ] **Step 3: Update `Mesh.h` — remove peer fields, add composed member**

Remove `PeerInfo` and `MasterInfo` struct definitions (now in `PeerRegistry.h`).

Add include:
```cpp
#include "PeerRegistry.h"
```

Remove from private section:
```cpp
// REMOVE:
PeerInfo peerMacs[MAX_PEERS];
size_t peerCount;
PeerInfo* findPeer(const uint8_t mac[6]);
bool isPeerInRange(const uint8_t mac[6]);
bool appendPeer(const PeerInfo& peer);
void loadPeersFromEEPROM();
void savePeersToEEPROM();
void addPeerToEEPROM(const uint8_t mac[6]);
void removePeerFromEEPROM(const uint8_t mac[6]);
void updatePeerLastSeen(const uint8_t mac[6]);
using lattice::utils::EEPROM_SIZES::MAX_PEERS;  // also remove if present
```

Add to private section:
```cpp
PeerRegistry peers;
```

Update public delegation methods:
```cpp
void addPeer(const uint8_t mac[6]) { peers.addAndPersist(mac); }
void removePeer(const uint8_t mac[6]) { peers.removeAndPersist(mac); }
const PeerInfo* getPeerList() const { return peers.peerMacs; }
size_t getPeerCount() const { return peers.peerCount; }
```

- [ ] **Step 4: Update `Mesh.cpp` — use `peers.` prefix**

Replace in `Mesh.cpp` (search-replace, careful not to touch `PeerRegistry.cpp`):
```
peerMacs[   →  peers.peerMacs[
peerCount   →  peers.peerCount
findPeer(   →  peers.find(
appendPeer( →  peers.append(
isPeerInRange(  →  peers.isPeerInRange(
updatePeerLastSeen(  →  peers.updateLastSeen(
loadPeersFromEEPROM()  →  peers.loadFromEEPROM()
savePeersToEEPROM()    →  peers.saveToEEPROM()
```

In `Mesh::init()`, after `readMacAddress()`, add:
```cpp
peers.setDeviceMac(deviceMacAddress);
```

`findNextHopToMaster()` stays in `Mesh.cpp` — it uses `currentMaster.nextHop`. Update its body to use `peers.`:
```cpp
PeerInfo* Mesh::findNextHopToMaster() {
  if (currentMaster.distance == 0xFF) return nullptr;
  for (size_t i = 0; i < peers.peerCount; ++i) {
    if (lattice::utils::MacAddress(peers.peerMacs[i].mac) ==
            lattice::utils::MacAddress(currentMaster.nextHop) &&
        peers.isPeerInRange(peers.peerMacs[i].mac) &&
        lattice::utils::MacAddress(peers.peerMacs[i].mac) !=
            lattice::utils::MacAddress(deviceMacAddress))
      return &peers.peerMacs[i];
  }
  return nullptr;
}
```

- [ ] **Step 5: Update `mesh_logic_impl.cpp` — remove moved methods, fix remaining**

**Remove** the following method bodies from `mesh_logic_impl.cpp` (they now live in `PeerRegistry.cpp` which is compiled in tests):
- `Mesh::appendPeer` (lines 167–172)
- `Mesh::findPeer` (lines 249–256)
- `Mesh::isPeerInRange` (lines 258–263)
- `Mesh::findNextHopToMaster` (lines 265–278)

**Update** remaining methods that reference peer fields directly — replace with `peers.` prefix:

In `Mesh::relayDownlink` (line 193–197): `peerCount` → `peers.peerCount`, `peerMacs[i].mac` → `peers.peerMacs[i].mac`

In `Mesh::broadcastToAllPeers` (line 280–290): same substitutions.

In `Mesh::transmitCore` (line 238): `findNextHopToMaster()` — this call stays as-is (it's a Mesh method).

In `Mesh::processAdapterData` (line 382–384): `hasMasterMac` is still a Mesh field at this point (Enrollment extraction is Task 7 — leave these alone for now).

- [ ] **Step 6: Update CMakeLists.txt**

Add to `FIRMWARE_SOURCES`:
```cmake
  ../main/src/mesh/PeerRegistry.cpp
```

- [ ] **Step 7: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "refactor: extract PeerRegistry from Mesh"
```

---

### Task 7: Extract `Enrollment`

Extract the enrollment protocol state machine. `Enrollment.cpp` has two mbedtls-heavy methods (`init` for key generation, `enrollPeer` for LMK derivation) — stub those in `firmware_stubs.cpp`. All other enrollment methods move to the production file and compile in tests.

**Files:**
- Create: `main/src/mesh/Enrollment.h`
- Create: `main/src/mesh/Enrollment.cpp`
- Modify: `main/src/mesh/Mesh.h`
- Modify: `main/src/mesh/Mesh.cpp`
- Modify: `tests/mocks/mesh_logic_impl.cpp` (remove moved methods; update `hasMasterMac` → `enrollment.hasMasterMac` etc.)
- Modify: `tests/mocks/firmware_stubs.cpp` (add stubs for `Enrollment::init` + `Enrollment::enrollPeer`)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MeshCrypto.h`, `EepromManager`
- Produces:
```cpp
namespace lattice::mesh {
using EnrollmentRelayFn = void (*)(const uint8_t mac[6], const uint8_t pubKey[32]);
using SendMessageFn = std::function<void(const uint8_t target[6], mesh_message)>;
using RegisterPeerFn = std::function<void(const uint8_t mac[6], const uint8_t* pubKey32)>;

class Enrollment {
public:
  // TOFU state (read by Mesh for beacon/config processing)
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};

  Enrollment();
  void init();   // loads or generates keypair; loads enrolled flag + TOFU MACs from EEPROM

  bool isEnrolled() const;
  const uint8_t* getPublicKey() const { return devicePublicKey; }
  const uint8_t* getPrivateKey() const { return devicePrivateKey; }

  void sendRequest(const uint8_t* deviceMac, SendMessageFn sendFn);
  void processRequest(const mesh_message& msg);
  void processJoinAck(const mesh_message& msg, const uint8_t* deviceMac,
                      RegisterPeerFn registerFn);
  void enrollPeer(const uint8_t mac[6], const uint8_t pubKey32[32],
                  RegisterPeerFn registerFn, bool dualMasterMode);

  void setRelayFn(EnrollmentRelayFn fn);
  void setPendingRelay(const uint8_t mac[6], const uint8_t pubKey[32]);
  void drainPendingRelay();

private:
  uint8_t devicePrivateKey[32]{};
  uint8_t devicePublicKey[32]{};
  volatile bool _pendingEnrollmentRelay{false};
  uint8_t _pendingEnrollmentMac[6]{};
  uint8_t _pendingEnrollmentPubKey[32]{};
  EnrollmentRelayFn _enrollmentRelayFn{nullptr};
};
}
```

- [ ] **Step 1: Create `main/src/mesh/Enrollment.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

using EnrollmentRelayFn = void (*)(const uint8_t mac[6], const uint8_t pubKey[32]);
using SendMessageFn = std::function<void(const uint8_t target[6], mesh_message)>;
using RegisterPeerFn = std::function<void(const uint8_t mac[6], const uint8_t* pubKey32)>;

class Enrollment {
public:
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};

  Enrollment();
  void init();

  bool isEnrolled() const;
  const uint8_t* getPublicKey() const { return devicePublicKey; }
  const uint8_t* getPrivateKey() const { return devicePrivateKey; }

  void sendRequest(const uint8_t* deviceMac, SendMessageFn sendFn);
  void processRequest(const mesh_message& msg);
  void processJoinAck(const mesh_message& msg, const uint8_t* deviceMac,
                      RegisterPeerFn registerFn);
  void enrollPeer(const uint8_t mac[6], const uint8_t pubKey32[32],
                  RegisterPeerFn registerFn, bool dualMasterMode);

  void setRelayFn(EnrollmentRelayFn fn);
  void setPendingRelay(const uint8_t mac[6], const uint8_t pubKey[32]);
  void drainPendingRelay();

private:
  uint8_t devicePrivateKey[32]{};
  uint8_t devicePublicKey[32]{};
  volatile bool _pendingEnrollmentRelay{false};
  uint8_t _pendingEnrollmentMac[6]{};
  uint8_t _pendingEnrollmentPubKey[32]{};
  EnrollmentRelayFn _enrollmentRelayFn{nullptr};
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Create `main/src/mesh/Enrollment.cpp`**

Move the following verbatim from `Mesh.cpp` (production implementation) and `mesh_logic_impl.cpp` (test implementation — same logic for these methods), replacing `Mesh::` → `Enrollment::`:

| Source | Destination |
|---|---|
| `Mesh::loadOrGenerateKeypair` | `Enrollment::init` — loads keypair from EEPROM + calls `crypto::generateKeypair` for the generation branch; also loads `enrolledFlag`, `knownMasterMac`, `knownMasterMacSecondary` |
| `Mesh::isEnrolled` | `Enrollment::isEnrolled` |
| `Mesh::sendEnrollmentRequest` (mesh_logic_impl) | `Enrollment::sendRequest` — takes `deviceMac` + `sendFn` params instead of accessing `Mesh::deviceMacAddress` and `Mesh::esp_now_send` directly |
| `Mesh::processEnrollmentRequest` (mesh_logic_impl) | `Enrollment::processRequest` |
| `Mesh::processJoinAck` (mesh_logic_impl) | `Enrollment::processJoinAck` — takes `deviceMac` + `registerFn` |
| `Mesh::enrollPeer` | `Enrollment::enrollPeer` — takes `registerFn` + `dualMasterMode` |
| `Mesh::setEnrollmentRelayFn` (mesh_logic_impl) | `Enrollment::setRelayFn` |
| `Mesh::drainPendingEnrollment` (mesh_logic_impl) | `Enrollment::drainPendingRelay` |

Signature adaptations for `sendRequest`:
```cpp
void Enrollment::sendRequest(const uint8_t* deviceMac, SendMessageFn sendFn) {
  mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  // ... exact body from Mesh::sendEnrollmentRequest
  // Replace: devicePublicKey → devicePublicKey (same, it's now 'this' field)
  // Replace: deviceMacAddress → deviceMac (parameter)
  // Replace: esp_now_send(broadcastMac, ...) → sendFn(broadcastMac, msg)  OR keep direct esp_now_send
  //   (checking mesh_logic_impl.cpp: it calls esp_now_send directly — keep that)
}
```

For `processJoinAck`, it was:
```cpp
// mesh_logic_impl.cpp:
void Mesh::processJoinAck(const mesh_message& msg) {
  if (memcmp(msg.target_mac_address, deviceMacAddress, 6) != 0) { relayDownlink(msg); return; }
  // ...
  if (!hasMasterMac) { memcpy(knownMasterMac, ...); hasMasterMac = true; ... }
}
```
The `relayDownlink` call must stay in `Mesh` — extract just the "addressed to us" branch:
```cpp
void Enrollment::processJoinAck(const mesh_message& msg, const uint8_t* deviceMac,
                                 RegisterPeerFn /*registerFn*/) {
  // Called only when msg.target_mac_address == deviceMacAddress (Mesh checks this before calling)
  if (memcmp(msg.data, devicePublicKey, 4) != 0) { /* log + return */ return; }
  EepromManager::getInstance().saveEnrolledFlag(true);
  if (!hasMasterMac) {
    memcpy(knownMasterMac, msg.origin_mac_address, 6);
    hasMasterMac = true;
    EepromManager::getInstance().saveKnownMasterMac(knownMasterMac);
  }
}
```
The relay check (`relayDownlink`) stays in Mesh.cpp `Mesh::drainRecvQueue`.

```cpp
#include "Enrollment.h"
#include "MeshCrypto.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "../../lib/lattice-protocol/c/opcodes.h"
#include <esp_now.h>

namespace lattice {
namespace mesh {

Enrollment::Enrollment() {
  memset(devicePrivateKey, 0, 32);
  memset(devicePublicKey, 0, 32);
  memset(knownMasterMac, 0xFF, 6);
  memset(knownMasterMacSecondary, 0xFF, 6);
  memset(_pendingEnrollmentMac, 0, 6);
  memset(_pendingEnrollmentPubKey, 0, 32);
}

// NOTE: Enrollment::init() and Enrollment::enrollPeer() use mbedtls.
// In test builds, these are STUBBED in firmware_stubs.cpp.
// The remaining methods compile cleanly without mbedtls.

// ... verbatim bodies
} // namespace mesh
} // namespace lattice
```

- [ ] **Step 3: Add stubs to `tests/mocks/firmware_stubs.cpp`**

Add at the bottom (after existing stubs):
```cpp
#include "mesh/Enrollment.h"

namespace lattice {
namespace mesh {

// Stub: key generation uses mbedtls — not available on host.
// Tests that call sendEnrollmentRequest() must set devicePublicKey directly via
// the UNIT_TEST public access (enrollment.devicePublicKey[...] = ...).
void Enrollment::init() {
  // Load enrolled flag from EEPROM (real impl)
  auto& em = lattice::utils::EepromManager::getInstance();
  // enrolled flag
  bool enrolled = em.loadEnrolledFlag();
  (void)enrolled;  // stored in test-accessible field — see Enrollment::isEnrolled()
  // TOFU MACs
  if (em.loadKnownMasterMac(knownMasterMac)) hasMasterMac = true;
  if (em.loadKnownMasterMacSecondary(knownMasterMacSecondary)) hasMasterMacSecondary = true;
  // Keys: leave zeroed (tests set them directly if needed)
}

void Enrollment::enrollPeer(const uint8_t mac[6], const uint8_t pubKey32[32],
                             RegisterPeerFn registerFn, bool /*dualMasterMode*/) {
  // Stub: skip LMK derivation — just invoke the callback with null key
  if (registerFn) registerFn(mac, pubKey32);
}

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 4: Update `Mesh.h`**

Add include:
```cpp
#include "Enrollment.h"
```

Remove from private section:
```cpp
// REMOVE:
uint8_t devicePrivateKey[32];
uint8_t devicePublicKey[32];
void loadOrGenerateKeypair();
uint8_t knownMasterMac[6];
bool hasMasterMac;
uint8_t knownMasterMacSecondary[6];
bool hasMasterMacSecondary;
volatile bool _pendingEnrollmentRelay;
uint8_t _pendingEnrollmentMac[6];
uint8_t _pendingEnrollmentPubKey[32];
EnrollmentRelayFn _enrollmentRelayFn;
void processEnrollmentRequest(const mesh_message& msg);
void processJoinAck(const mesh_message& msg);
void drainPendingEnrollment();
```

Remove `EnrollmentRelayFn` typedef (it now lives in `Enrollment.h`).

Add to private section:
```cpp
Enrollment enrollment;
```

Update public delegation methods:
```cpp
void sendEnrollmentRequest() {
  enrollment.sendRequest(deviceMacAddress,
    [this](const uint8_t* t, mesh_message m){ this->sendMessage(t, m); });
}
bool isEnrolled() const { return enrollment.isEnrolled(); }
void enrollPeer(const uint8_t mac[6], const uint8_t pubKey[32]);  // impl in Mesh.cpp
void setEnrollmentRelayFn(EnrollmentRelayFn fn) { enrollment.setRelayFn(fn); }
const uint8_t* getDevicePublicKey() const { return enrollment.getPublicKey(); }
```

- [ ] **Step 5: Update `Mesh.cpp`**

- Remove `Mesh::loadOrGenerateKeypair`, `Mesh::isEnrolled`, `Mesh::sendEnrollmentRequest`, `Mesh::processEnrollmentRequest`, `Mesh::processJoinAck`, `Mesh::enrollPeer`, `Mesh::setEnrollmentRelayFn`, `Mesh::drainPendingEnrollment`.
- In `Mesh::init()`, replace `loadOrGenerateKeypair()` with `enrollment.init()`.
- In `Mesh::loadPersistentState()`, replace direct key/enrollment field reads using `enrollment.`.
- In `Mesh::drainRecvQueue()`:
  - `processEnrollmentRequest(msg)` → `enrollment.processRequest(msg)`
  - `processJoinAck(msg)` → check target first, then `enrollment.processJoinAck(msg, deviceMacAddress, [this](...){})`; relay via `relayDownlink` if not addressed to us
  - `drainPendingEnrollment()` → `enrollment.drainPendingRelay()`
- In `Mesh::processMasterBeacon()`: replace `hasMasterMac`, `knownMasterMac`, `hasMasterMacSecondary`, `knownMasterMacSecondary` with `enrollment.hasMasterMac` etc.
- In `Mesh::processAdapterData()`: same for `hasMasterMac`, `knownMasterMac` references.
- `Mesh::enrollPeer` thin wrapper:
```cpp
void Mesh::enrollPeer(const uint8_t mac[6], const uint8_t pubKey[32]) {
  enrollment.enrollPeer(mac, pubKey,
    [this](const uint8_t* m, const uint8_t* k) {
      lattice::mesh::crypto::registerPeerWithEspNow(m, enrollment.getPrivateKey(), k);
      peers.addAndPersist(m);
    },
    _dualMasterMode);
}
```
- Print public key in `Mesh::init()` (was using `devicePublicKey` directly): → `enrollment.getPublicKey()`

- [ ] **Step 6: Update `mesh_logic_impl.cpp` — remove moved methods, fix remaining**

**Remove** entirely (they now live in `Enrollment.cpp` which is compiled in tests):
- `Mesh::sendEnrollmentRequest` (lines 401–414)
- `Mesh::processEnrollmentRequest` (lines 416–425)
- `Mesh::setEnrollmentRelayFn` (lines 427–429)
- `Mesh::drainPendingEnrollment` (lines 431–438)

**Update** `processJoinAck` (lines 200–221): it remains in `mesh_logic_impl.cpp` OR can be removed if `Enrollment::processJoinAck` covers it. Since the relay-check part stays in Mesh, keep only the relay dispatch and call through:
```cpp
void Mesh::processJoinAck(const mesh_message& msg) {
  if (memcmp(msg.target_mac_address, deviceMacAddress, 6) != 0) {
    relayDownlink(msg);
    return;
  }
  enrollment.processJoinAck(msg, deviceMacAddress, nullptr);
}
```

**Update** `processMasterBeacon`: replace all direct `hasMasterMac`, `knownMasterMac`, `hasMasterMacSecondary`, `knownMasterMacSecondary`, `_dualMasterMode` with `enrollment.hasMasterMac` etc.

**Update** `processAdapterData`: replace `hasMasterMac`, `knownMasterMac` → `enrollment.hasMasterMac`, `enrollment.knownMasterMac`.

- [ ] **Step 7: Update CMakeLists.txt**

Add to `FIRMWARE_SOURCES`:
```cmake
  ../main/src/mesh/Enrollment.cpp
```

- [ ] **Step 8: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "refactor: extract Enrollment from Mesh"
```

---

### Task 8: Extract `SerialFraming`

Extract the framing state machine from `SerialAdapter` into a testable class. Currently `injectByte` is `#ifdef UNIT_TEST` only in `SerialAdapter`; in `SerialFraming` it's always public (cleaner).

**Files:**
- Create: `main/src/adapter/serial/SerialFraming.h`
- Create: `main/src/adapter/serial/SerialFraming.cpp`
- Modify: `main/src/adapter/serial/SerialAdapter.h` (remove framing fields/methods, add `SerialFraming _framing` member)
- Modify: `main/src/adapter/serial/SerialAdapter.cpp`
- Modify: `tests/CMakeLists.txt`

Current framing state in `SerialAdapter.h` (lines 53–58):
```cpp
enum class FrameState : uint8_t { AwaitingLen1, AwaitingLen2, AwaitingPayload };
FrameState frameState;
uint16_t frameLength;
size_t frameIndex;
static constexpr size_t MAX_PAYLOAD = 256;
uint8_t payloadBuffer[MAX_PAYLOAD];
```

**Interfaces:**
- Produces:
```cpp
namespace lattice::adapter::serial {
class SerialFraming {
public:
  static size_t encode(const lattice::mesh::mesh_message& msg, uint8_t* out, size_t maxLen);
  static bool decode(const uint8_t* data, size_t len, lattice::mesh::mesh_message& out);
  bool injectByte(uint8_t b);  // always public (was UNIT_TEST-only in SerialAdapter)
  const uint8_t* frameBuffer() const { return payloadBuffer; }
  size_t frameLen() const { return frameIndex; }  // valid after injectByte returns true
private:
  enum class FrameState : uint8_t { AwaitingLen1, AwaitingLen2, AwaitingPayload };
  FrameState frameState{FrameState::AwaitingLen1};
  uint16_t frameLength{0};
  size_t frameIndex{0};
  static constexpr size_t MAX_PAYLOAD = 256;
  uint8_t payloadBuffer[MAX_PAYLOAD]{};
};
}
```

- [ ] **Step 1: Create `main/src/adapter/serial/SerialFraming.h`**

```cpp
#pragma once
#include <cstdint>
#include "../../mesh/serialization/mesh.pb.h"
#include "src/mesh/Mesh.h"  // for mesh_message

namespace lattice {
namespace adapter {
namespace serial {

class SerialFraming {
public:
  static size_t encode(const lattice::mesh::mesh_message& msg, uint8_t* out, size_t maxLen);
  static bool decode(const uint8_t* data, size_t len, lattice::mesh::mesh_message& out);

  // Feed one byte into the framing state machine.
  // Returns true when a complete frame is ready; read it via frameBuffer()/frameLen().
  bool injectByte(uint8_t b);

  const uint8_t* frameBuffer() const { return payloadBuffer; }
  size_t frameLen() const { return frameIndex; }

private:
  enum class FrameState : uint8_t { AwaitingLen1, AwaitingLen2, AwaitingPayload };
  FrameState frameState{FrameState::AwaitingLen1};
  uint16_t frameLength{0};
  size_t frameIndex{0};
  static constexpr size_t MAX_PAYLOAD = 256;
  uint8_t payloadBuffer[MAX_PAYLOAD]{};
};

} // namespace serial
} // namespace adapter
} // namespace lattice
```

- [ ] **Step 2: Create `main/src/adapter/serial/SerialFraming.cpp`**

Move method bodies verbatim from `SerialAdapter.cpp`:
- `Serial_Adapter::encodeMeshMessage` → `SerialFraming::encode` (static)
- `Serial_Adapter::decodeMeshMessage` → `SerialFraming::decode` (static)
- `Serial_Adapter::injectByte` → `SerialFraming::injectByte` (replace `frameState`, `frameLength`, `frameIndex`, `payloadBuffer` — same field names, now on `this`)

```cpp
#include "SerialFraming.h"
#include "src/mesh/serialization/mesh.pb.h"
#include "src/mesh/serialization/nanopb/pb_encode.h"
#include "src/mesh/serialization/nanopb/pb_decode.h"

namespace lattice {
namespace adapter {
namespace serial {

size_t SerialFraming::encode(const lattice::mesh::mesh_message& msg, uint8_t* out, size_t maxLen) {
  // verbatim from Serial_Adapter::encodeMeshMessage
}

bool SerialFraming::decode(const uint8_t* data, size_t len, lattice::mesh::mesh_message& out) {
  // verbatim from Serial_Adapter::decodeMeshMessage
}

bool SerialFraming::injectByte(uint8_t b) {
  // verbatim from Serial_Adapter::injectByte
}

} // namespace serial
} // namespace adapter
} // namespace lattice
```

- [ ] **Step 3: Update `SerialAdapter.h`**

Add include:
```cpp
#include "SerialFraming.h"
```

Remove from private section:
```cpp
// REMOVE:
enum class FrameState : uint8_t { AwaitingLen1, AwaitingLen2, AwaitingPayload };
FrameState frameState;
uint16_t frameLength;
size_t frameIndex;
static constexpr size_t MAX_PAYLOAD = 256;
uint8_t payloadBuffer[MAX_PAYLOAD];
static size_t encodeMeshMessage(...);
static bool decodeMeshMessage(...);
```

Remove the `#ifdef UNIT_TEST` block for `injectByte` / `lastOpcode` (testing now goes through `SerialFraming`).

Add to private section:
```cpp
SerialFraming _framing;
```

- [ ] **Step 4: Update `SerialAdapter.cpp`**

Replace method calls:
```cpp
// OLD (in loop()):
if (injectByte(b)) { handleCompleteFrame(payloadBuffer, frameIndex); }
// NEW:
if (_framing.injectByte(b)) { handleCompleteFrame(_framing.frameBuffer(), _framing.frameLen()); }

// OLD (in sendHealthReport / onMeshDataImpl):
size_t len = encodeMeshMessage(msg, buf, sizeof(buf));
// NEW:
size_t len = SerialFraming::encode(msg, buf, sizeof(buf));

// OLD (in handleCompleteFrame):
if (!decodeMeshMessage(data, len, msg)) ...
// NEW:
if (!SerialFraming::decode(data, len, msg)) ...
```

Also remove `Serial_Adapter::injectByte` and `Serial_Adapter::lastOpcode` definitions.

- [ ] **Step 5: Update `test_serial_framing.cpp`**

The test currently accesses `Serial_Adapter::injectByte` via `UNIT_TEST`. Update it to instantiate `SerialFraming` directly:

```cpp
#include <gtest/gtest.h>
#include "adapter/serial/SerialFraming.h"

using namespace lattice::adapter::serial;

// Test encode → decode round-trip
TEST(SerialFramingTest, EncodeDecodeRoundTrip) {
  lattice::mesh::mesh_message original{};
  original.message_type = MESH_TYPE_ADAPTER_DATA;
  memset(original.origin_mac_address, 0x11, 6);
  original.data[0] = 0x42;

  uint8_t buf[256];
  size_t len = SerialFraming::encode(original, buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  lattice::mesh::mesh_message decoded{};
  EXPECT_TRUE(SerialFraming::decode(buf, len, decoded));
  EXPECT_EQ(decoded.message_type, original.message_type);
  EXPECT_EQ(decoded.data[0], original.data[0]);
  EXPECT_EQ(memcmp(decoded.origin_mac_address, original.origin_mac_address, 6), 0);
}

// Test injectByte state machine: feed framed bytes one at a time
TEST(SerialFramingTest, InjectByteReassemblesFrame) {
  lattice::mesh::mesh_message original{};
  original.message_type = MESH_TYPE_MASTER_BEACON;

  uint8_t buf[256];
  size_t len = SerialFraming::encode(original, buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  SerialFraming framing;
  bool complete = false;
  for (size_t i = 0; i < len; ++i) {
    complete = framing.injectByte(buf[i]);
  }
  EXPECT_TRUE(complete);

  lattice::mesh::mesh_message decoded{};
  EXPECT_TRUE(SerialFraming::decode(framing.frameBuffer(), framing.frameLen(), decoded));
  EXPECT_EQ(decoded.message_type, original.message_type);
}
```

Retain any additional test cases from the original `test_serial_framing.cpp`.

- [ ] **Step 6: Update CMakeLists.txt**

Add to `FIRMWARE_SOURCES`:
```cmake
  ../main/src/adapter/serial/SerialFraming.cpp
```

- [ ] **Step 7: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass including `test_serial_framing`.

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "refactor: extract SerialFraming from SerialAdapter"
```

---

### Task 9: Create `src/app/` layer + reduce `main.ino`

Extract three inline state machines from `main.ino` into header-only classes. No new `.cpp` files — header-only. No CMakeLists change needed.

**Files:**
- Create: `main/src/app/BootManager.h`
- Create: `main/src/app/DisplayManager.h`
- Create: `main/src/app/ButtonHandler.h`
- Modify: `main/main.ino`

- [ ] **Step 1: Create `main/src/app/BootManager.h`**

The body is the verbatim reset-reason block from `main.ino setup()` (the `esp_reset_reason_t reason = ...` block, ~lines 93–111). EEPROM must already be initialised before calling (same constraint as today).

```cpp
#pragma once
#include <esp_system.h>
#include <Arduino.h>
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace app {

struct BootManager {
  static void check(lattice::utils::EepromManager& em) {
    esp_reset_reason_t reason = esp_reset_reason();
    em.saveRebootReason(static_cast<uint8_t>(reason));
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
      uint8_t count = em.loadRebootCount();
      count++;
      em.saveRebootCount(count);
      Serial.printf("[BOOT] WDT reset #%d (reason: %d)\n", count, (int)reason);
      if (count >= 5) {
        Serial.println("[BOOT] WDT loop detected — halting. Manual reset required.");
        while (true) { delay(1000); }
      }
    } else {
      em.saveRebootCount(0);
    }
  }
};

} // namespace app
} // namespace lattice
```

- [ ] **Step 2: Create `main/src/app/DisplayManager.h`**

Extract the `if (lattice::config::ENABLE_SEVSEG_DISPLAY)` block from `main.ino loop()` (lines ~312–342). The `static` local variables move inside the function — valid in a header-only inline static method.

```cpp
#pragma once
#include <cstdint>
#include <Arduino.h>
#include "src/hardware/output/SevenSegDisplay.h"

namespace lattice {
namespace app {

struct DisplayManager {
  static void tick(lattice::hardware::SevenSegDisplay& display,
                   bool enrolled, bool isMaster, uint8_t nodeId) {
    static uint32_t lastToggleMs = 0;
    static bool dashVisible = false;

    if (!enrolled) {
      if (millis() - lastToggleMs >= 500) {
        lastToggleMs = static_cast<uint32_t>(millis());
        dashVisible = !dashVisible;
        if (dashVisible) {
          static const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
          display.setSegments(dashes);
        } else {
          display.clear();
        }
      }
    } else if (nodeId == 0) {
      display.show(0, false);
    } else if (isMaster) {
      display.showWithDP(static_cast<int>(nodeId), false);
    } else {
      display.show(static_cast<int>(nodeId), false);
    }
  }
};

} // namespace app
} // namespace lattice
```

- [ ] **Step 3: Create `main/src/app/ButtonHandler.h`**

Extract both button state machines from `main.ino loop()` (lines ~364–430 for config button, ~368–430 for reset button). Static locals move into the private tick methods.

```cpp
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

  static void tick(lattice::hardware::Button& configBtn,
                   lattice::hardware::Button& resetBtn,
                   lattice::mesh::Mesh& mesh,
                   lattice::utils::EepromManager& em,
                   lattice::hardware::Led& greenLed,
                   lattice::hardware::Led& redLed,
                   bool isDevMode,
                   bool& devMasterFlag) {
    tickConfig(configBtn, mesh, em, greenLed, isDevMode, devMasterFlag);
    tickReset(resetBtn, em, greenLed, redLed);
  }

private:
  static void tickConfig(lattice::hardware::Button& btn, lattice::mesh::Mesh& mesh,
                         lattice::utils::EepromManager& em,
                         lattice::hardware::Led& greenLed,
                         bool isDevMode, bool& devMasterFlag) {
    static bool wasPressed = false;
    static unsigned long holdStart = 0;
    // verbatim config button block from main.ino loop() — lines ~371–397
    // Replace 'BUTTON_HOLD_TIME_MS' with 'HOLD_MS'
  }

  static void tickReset(lattice::hardware::Button& btn,
                        lattice::utils::EepromManager& em,
                        lattice::hardware::Led& greenLed,
                        lattice::hardware::Led& redLed) {
    static bool wasPressed = false;
    static unsigned long holdStart = 0;
    static bool confirmPending = false;
    static uint32_t confirmDeadline = 0;
    // verbatim reset button block from main.ino loop() — lines ~401–430
    // Replace 'BUTTON_HOLD_TIME_MS' with 'HOLD_MS'
  }
};

} // namespace app
} // namespace lattice
```

- [ ] **Step 4: Update `main.ino`**

Add includes at top of `main.ino`:
```cpp
#include "src/app/BootManager.h"
#include "src/app/DisplayManager.h"
#include "src/app/ButtonHandler.h"
```

In `setup()`, replace the reset-reason block (`esp_reset_reason_t reason = ...` through `em.saveRebootCount(0)`) with:
```cpp
lattice::app::BootManager::check(EepromManager::getInstance());
```

In `loop()`, replace the display block with:
```cpp
if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
  bool enrolled = mesh.isEnrolled() || mesh.getIsMaster();
  uint8_t nodeId = lattice::utils::EepromManager::getInstance().loadNodeId();
  lattice::app::DisplayManager::tick(sevenSeg, enrolled, mesh.getIsMaster(), nodeId);
}
```

Replace both button blocks with:
```cpp
lattice::app::ButtonHandler::tick(configButton, resetButton, mesh,
  lattice::utils::EepromManager::getInstance(),
  greenLed, redLed, isDevMode, devMasterFlag);
```

- [ ] **Step 5: Verify `main.ino` line count**

```bash
wc -l main/main.ino
```

Expected: ≤ 160 lines.

- [ ] **Step 6: Run tests**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass. (app/ has no unit tests — it wraps Arduino hardware.)

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "refactor: extract app/ layer from main.ino"
```

---

### Task 10: Update `REFACTORING_GUIDE.md` + final verification

**Files:**
- Modify: `REFACTORING_GUIDE.md`

- [ ] **Step 1: Rewrite `REFACTORING_GUIDE.md` module map section**

Replace the `## Module Map` section with:

```markdown
## Module Map

### `main/main.ino`
Wiring only: initialises hardware in dependency order, wires callbacks, runs main loop.
Delegates all logic to `src/app/` components.

### `main/project_config.h`
Single source of truth for every compile-time constant. Edit here only.

### `src/app/`
Application-level coordination extracted from `main.ino`. Header-only — no `.cpp` files.
- `BootManager` — reset reason check, WDT loop detection, reboot counter.
- `DisplayManager` — 7-seg enrolled/unenrolled/master state machine.
- `ButtonHandler` — config role-toggle and double-confirm EEPROM-wipe state machines.

### `src/mesh/`
- `Mesh` — thin orchestrator: ESP-NOW radio init, lock-free SPSC recv queue, routing dispatch, beacon relay, `loop()`.
- `PeerRegistry` — peer list CRUD, EEPROM serialisation, staleness checks.
- `Enrollment` — Curve25519 keypair load/generate, enrollment broadcast, JOIN_ACK processing, TOFU master MAC persistence.
- `ReplayCache` — per-boot epoch + sequence-number replay protection (header-only struct).
- `MeshCrypto` — ECDH shared-secret derivation, peer LMK, keypair generation, ESP-NOW peer registration (header-only inline functions).
- `serialization/` — nanopb-generated `mesh.pb.c/.h` + nanopb runtime. Do not hand-edit.

### `src/adapter/`
- `Adapter` — abstract base: owns `mesh_transmit_fn`, handles `OP_CONFIG_SET` for all adapter types.
- `AdapterFactory` — creates adapters from EEPROM type byte; provides default pins.
- `pir/PirAdapter` — HC-SR501 motion events → mesh broadcast.
- `serial/SerialAdapter` — framed Protobuf I/O to host server; health reports.
- `serial/SerialFraming` — 2-byte-length-prefixed protobuf encode/decode + byte-feed state machine. Testable without hardware.

### `src/hardware/`
- `GpioOutput` / `GpioInput` — pin-validation and `_initialized` guard. All single-pin peripherals inherit from one.
- `Led`, `SevenSegDisplay`, `Button`, `Pir` — single-pin peripheral drivers.

### `src/logging/`
Levelled logging (`LOG_DEBUG` → `LOG_NONE`). Set `DEFAULT_LOG_LEVEL = LOG_NONE` when the serial port is used for host-server framing.

### `src/error/`
- `Error.h` — public API: `lattice::err::fail()` / `lattice::err::fatal()`.
- `ErrorCore` — error LED blink pattern and TM1637 display.
- `ErrorCodes.h` — numeric `T·M·S` code registry.

### `src/persistence/`
`EepromManager` — all EEPROM reads/writes through singleton. `DEV_MODE` no-ops all writes. Centralises address constants in `EEPROM_ADDRESSES::*`.

### `src/network/`
`MacAddress` — MAC comparison, formatting, zero-checking utilities.

## Adding a Module

1. Place it under the most relevant `src/<subsystem>/` directory.
2. Single responsibility — if it touches two concerns, split it.
3. Route all errors through `src/error/Error.h`.
4. Use `GpioOutput` / `GpioInput` for new single-pin hardware drivers.
5. Reserve dynamic containers at `setup()` time; never grow them in `loop()`.
6. Add new `.cpp` files to `tests/CMakeLists.txt` `FIRMWARE_SOURCES` if they are hardware-independent (no mbedtls). Add stubs to `tests/mocks/firmware_stubs.cpp` for any mbedtls-using methods.
```

- [ ] **Step 2: Run full test suite**

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure -V
```

Expected: **all tests pass, zero failures**.

- [ ] **Step 3: Commit**

```bash
git add REFACTORING_GUIDE.md && git commit -m "docs: update REFACTORING_GUIDE for restructured layout"
```
