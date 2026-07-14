# E2E Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multi-node mesh e2e simulation on host (no hardware) covering enrollment, multi-hop relay, route reporting, replay protection, dual-master failover, adapter hotswap, PIR data flow, PIR health, and serial framing — plus fixing the unit tests to compile the real `Mesh.cpp`/`Enrollment.cpp` instead of the `mesh_logic_impl.cpp` reimplementation.

**Architecture:** Single test process holds N simulated nodes. Each `SimNode` owns real firmware objects (`Mesh`, adapter via `AdapterFactory`, `Enrollment` inside `Mesh`) and a `NodeContext` snapshot of every mutable global (EEPROM image, serial buffers, ESP-NOW mock state, device MAC, firmware statics). A scheduler swaps each node's context in, runs one loop-equivalent `tick()`, swaps out; a `VirtualBus` with a topology graph routes captured ESP-NOW sends to reachable nodes; a `FakeHub` speaks the length-prefixed nanopb serial protocol against the master node. Virtual clock (`_mockMillis`), fully deterministic.

**Tech Stack:** C++17, GoogleTest 1.14 (existing), CMake ≥3.16, mbedtls 3.6.x via FetchContent (host build, pinned hash), existing mock layer in `tests/mocks/`.

**Spec:** `docs/superpowers/specs/2026-07-14-e2e-test-suite-design.md`

## Global Constraints

- All FetchContent dependencies pinned by tag URL + `URL_HASH SHA256` (pattern: googletest in `tests/CMakeLists.txt`).
- `UNIT_TEST` is defined globally in test builds (makes private members public in `Mesh`/`Enrollment`); e2e target additionally defines `SIMULATE_MODE=1`.
- Mocks shadow real SDK headers via include order (`tests/mocks` first). Never add an include path that lets real ESP-IDF headers in.
- Firmware source changes must be test-gated (`#ifdef UNIT_TEST`) — no behavior change in device builds. Run `clang-format` on any touched `main/` file (CI lints).
- Test-side code (tests/, harness) follows existing test style: plain gtest, no gmock unless already used.
- Existing unit tests must stay green after every task. Command: `cmake -B tests/build tests/ && cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure`.
- E2E tests get ctest label `e2e`; the PR workflow excludes them (`--label-exclude e2e`); a new manual (`workflow_dispatch`) workflow runs them.
- New files live under `tests/e2e/`; nothing in `main/` may include anything from `tests/`.

---

### Task 1: Compile real Mesh.cpp + Enrollment.cpp in the unit build (kill mesh_logic_impl.cpp)

The unit build currently links `tests/mocks/mesh_logic_impl.cpp` (436-line reimplementation of Mesh/Enrollment methods) and stubs crypto. Replace with the real sources + real mbedtls.

**Files:**
- Modify: `tests/CMakeLists.txt`
- Delete: `tests/mocks/mesh_logic_impl.cpp`
- Modify: `tests/mocks/firmware_stubs.cpp` (shrink to nothing or delete — see step 4)
- Test: existing `tests/unit/*.cpp` (7 files) are the regression suite

**Interfaces:**
- Produces: unit + e2e builds where `lattice::mesh::Mesh` and `lattice::mesh::Enrollment` are the real implementations, and `lattice::mesh::crypto::generateKeypair` / `registerPeerWithEspNow` (MeshCrypto.h) work on host via mbedtls. Later tasks (FakeHub key generation, enrollment e2e) depend on this.

- [ ] **Step 1: Pin mbedtls hash**

Run:
```bash
curl -sL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 | shasum -a 256
```
Record the hash for step 2. (If the URL 404s, use the latest 3.6.x release asset and note the version in the commit message.)

- [ ] **Step 2: Add mbedtls to tests/CMakeLists.txt**

Insert after the googletest `FetchContent_MakeAvailable`:

```cmake
# mbedtls — real Curve25519 for Enrollment/MeshCrypto on host
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)      # mbedtls' own tests
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=<hash-from-step-1>
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(mbedtls)
```

Note: mbedtls defines `ENABLE_TESTING` too — setting it OFF before `FetchContent_MakeAvailable(mbedtls)` but AFTER googletest is processed avoids disabling gtest discovery. Keep `enable_testing()` for the project itself where it already is.

- [ ] **Step 3: Swap sources**

In `FIRMWARE_SOURCES`: remove `mocks/mesh_logic_impl.cpp`; add:
```cmake
  ../main/src/mesh/Mesh.cpp
  ../main/src/mesh/Enrollment.cpp
```
In `add_unit_test` macro, link mbedtls:
```cmake
  target_link_libraries(${name} gtest_main gmock mbedcrypto)
```
Delete `tests/mocks/mesh_logic_impl.cpp` (git rm).

- [ ] **Step 4: Shrink firmware_stubs.cpp**

Every Mesh/Enrollment stub in `tests/mocks/firmware_stubs.cpp` now collides with the real definitions. Delete all of them, including `Mesh* Mesh::instance = nullptr;` (Mesh.cpp defines it). After deletion the file is likely empty → `git rm tests/mocks/firmware_stubs.cpp` and drop it from `FIRMWARE_SOURCES`. If the build then fails on a genuinely hardware-only symbol (e.g. something from `esp_bt.h`), re-create the file containing only that stub with a one-line comment.

- [ ] **Step 5: Build and fix compile errors**

Run:
```bash
rm -rf tests/build && cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --parallel
```
Expected failure classes and fixes (fix in mocks, not firmware, unless test-gated):
- Missing mock symbols Mesh.cpp needs (e.g. `esp_err_to_name`, `setCpuFrequencyMhz`): add inline no-op to the matching mock header.
- mbedtls include path: MeshCrypto.h includes `<mbedtls/ecdh.h>` — `mbedcrypto` target provides it transitively; if not, add `target_include_directories` with `${mbedtls_SOURCE_DIR}/include`.
- Duplicate symbol = a stub you missed in step 4.

- [ ] **Step 6: Run unit tests; reconcile drift**

Run: `ctest --test-dir tests/build --output-on-failure`

`mesh_logic_impl.cpp` was a reimplementation, so expect some assertion failures where it drifted from real `Mesh.cpp`. Rule: the real firmware behavior is the truth — update test expectations to match real behavior. Exception: if a test exposes an actual firmware bug (e.g. real code violates the documented protocol in README/spec), STOP and report to the user instead of changing either side. Tests that set `enrollment.devicePublicKey` manually (because init() was stubbed) may now find keys are real — such setup lines can be deleted.

Expected end state: all 7 test executables PASS.

- [ ] **Step 7: Commit**

```bash
git add -A tests/
git commit -m "test: compile real Mesh.cpp/Enrollment.cpp with mbedtls in unit build

Deletes the mesh_logic_impl.cpp reimplementation so unit tests exercise
the real mesh code path. Crypto is real (mbedtls host build)."
```

---

### Task 2: Mock upgrades — serial RX path, ESP.restart flag, fatal-error hooks

**Files:**
- Modify: `tests/mocks/serial_mock.h`, `tests/mocks/serial_mock.cpp`
- Modify: `tests/mocks/Arduino.h` (ESPClass)
- Modify: `main/src/error/Error.h` (UNIT_TEST-gated throw)
- Modify: `main/src/error/ErrorCore.cpp` (`restartDevice` UNIT_TEST gate)
- Create: `tests/unit/test_mocks.cpp`
- Modify: `tests/CMakeLists.txt` (register test)

**Interfaces:**
- Produces:
  - `SerialClass::injectRx(const uint8_t* data, size_t n)`, `available()`, `read()` backed by `std::deque<uint8_t> rxQueue` (public member, swappable).
  - `ESPClass::_restartRequested` (bool, public) set by `restart()`.
  - `lattice::err::FatalError : std::runtime_error` thrown by `err::fatal()` in UNIT_TEST builds (declared in `Error.h`).
  - Global counter `int lattice_test_errFailCount` (defined in `tests/mocks/Arduino.cpp`, declared `extern` in `Error.h` under UNIT_TEST) incremented by `err::fail()`.

- [ ] **Step 1: Write failing tests**

`tests/unit/test_mocks.cpp`:
```cpp
#include <gtest/gtest.h>
#include "Arduino.h"
#include "serial_mock.h"
#include "src/error/Error.h"

TEST(SerialMock, RxQueueRoundTrip) {
  Serial.reset();
  uint8_t bytes[3] = {0xAA, 0xBB, 0xCC};
  Serial.injectRx(bytes, 3);
  ASSERT_EQ(Serial.available(), 3);
  EXPECT_EQ(Serial.read(), 0xAA);
  EXPECT_EQ(Serial.read(), 0xBB);
  EXPECT_EQ(Serial.read(), 0xCC);
  EXPECT_EQ(Serial.available(), 0);
  EXPECT_EQ(Serial.read(), -1);
}

TEST(EspMock, RestartSetsFlag) {
  ESP._restartRequested = false;
  ESP.restart();
  EXPECT_TRUE(ESP._restartRequested);
}

TEST(ErrorHooks, FatalThrows) {
  EXPECT_THROW(
      lattice::err::fatal(lattice::core::ErrorTypeDigit::GENERIC,
                          lattice::core::ModuleDigit::CORE, 9, "boom"),
      lattice::err::FatalError);
}

TEST(ErrorHooks, FailIncrementsCounter) {
  int before = lattice_test_errFailCount;
  lattice::err::fail(lattice::utils::ErrorType::GENERIC, "soft");
  EXPECT_EQ(lattice_test_errFailCount, before + 1);
}
```
Register in `tests/CMakeLists.txt`: `add_unit_test(test_mocks unit/test_mocks.cpp)`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build tests/build --parallel 2>&1 | tail -5`
Expected: compile error — `injectRx` not a member.

- [ ] **Step 3: Implement**

`serial_mock.h` — replace `available()`/`read()`, add rx queue (include `<deque>`):
```cpp
  std::deque<uint8_t> rxQueue;

  void injectRx(const uint8_t* data, size_t n) { rxQueue.insert(rxQueue.end(), data, data + n); }
  int available() { return static_cast<int>(rxQueue.size()); }
  int read() {
    if (rxQueue.empty()) return -1;
    uint8_t b = rxQueue.front();
    rxQueue.pop_front();
    return b;
  }
  // also add printf (SIMULATE_MODE dump uses Serial.printf)
  void printf(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
  }
```
Extend `reset()`: `rxQueue.clear();`.

`Arduino.h` ESPClass:
```cpp
struct ESPClass {
  bool _restartRequested = false;
  void restart() { _restartRequested = true; }
  uint32_t getFreeHeap() { return 200000; }
};
```

`main/src/error/Error.h` — inside `namespace err`, before `fatal`:
```cpp
#ifdef UNIT_TEST
#include <stdexcept>
namespace lattice { namespace err {
class FatalError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
}} // namespace lattice::err
extern int lattice_test_errFailCount;
#endif
```
(Place the `#include <stdexcept>` at the top of the file with the other includes, gated the same way; the class inside the existing `namespace err` block rather than reopening namespaces if simpler.)

In `fail(digit...)` overload, first line:
```cpp
#ifdef UNIT_TEST
  ++lattice_test_errFailCount;
#endif
```
In both `fatal` overloads, replace the `while (true) {}` path:
```cpp
#ifdef UNIT_TEST
  throw FatalError(msg ? msg : "fatal");
#else
  while (true) {
  }
#endif
```
Note: `[[noreturn]]` stays correct — throwing satisfies it.

`main/src/error/ErrorCore.cpp` `restartDevice()`:
```cpp
#ifdef UNIT_TEST
  throw lattice::err::FatalError("ErrorCore::restartDevice");
#else
  ESP.restart();
#endif
```
(Include `Error.h` if not already; keep whatever follows unreachable.)

Define the counter in `tests/mocks/Arduino.cpp`:
```cpp
int lattice_test_errFailCount = 0;
```

Run `clang-format -i main/src/error/Error.h main/src/error/ErrorCore.cpp`.

- [ ] **Step 4: Run tests**

Run: `cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure`
Expected: all PASS, including the 4 new tests. Watch for existing tests that relied on `err::fatal` NOT throwing — wrap their trigger in `EXPECT_THROW` if any fail.

- [ ] **Step 5: Commit**

```bash
git add tests/ main/src/error/
git commit -m "test: serial RX mock, ESP.restart flag, UNIT_TEST fatal-error hooks"
```

---

### Task 3: SimClock + NodeContext global swap

**Files:**
- Create: `tests/e2e/harness/SimClock.h`
- Create: `tests/e2e/harness/NodeContext.h`, `tests/e2e/harness/NodeContext.cpp`
- Create: `tests/e2e/scenarios/test_harness_smoke.cpp`
- Modify: `tests/CMakeLists.txt` (e2e target — see step 3)
- Modify: `tests/mocks/esp_now_mock.h/.cpp` (expose recv callback for swapping)

**Interfaces:**
- Consumes: mock globals (`EEPROM`, `Serial`, `ESP`, `_mockMillis`, `mockDeviceMac`, `espNowSentPackets`, `espNowRegisteredPeers`), firmware statics (`lattice::mesh::Mesh::instance` — public under UNIT_TEST; `lattice::adapter::PirAdapter::instance` — private: add UNIT_TEST public gate like Mesh's, plus `SerialAdapter::lastHealthMillis` same treatment).
- Produces:
  - `sim::SimClock` — `void advance(uint32_t ms)` (increments `_mockMillis`), `uint32_t now()`.
  - `struct sim::NodeContext` — all fields public; default EEPROM image = all `0xFF`.
  - `void sim::swapIn(NodeContext&)` / `void sim::swapOut(NodeContext&)` — load/capture ALL per-node globals.
  - esp_now mock: `EspNowRecvCb getEspNowRecvCb(); void setEspNowRecvCb(EspNowRecvCb);` where `using EspNowRecvCb = void (*)(const esp_now_recv_info*, const uint8_t*, int);`.

- [ ] **Step 1: Expose swappable statics**

`tests/mocks/esp_now_mock.h`: add the `EspNowRecvCb` alias + getter/setter declarations; implement in `esp_now_mock.cpp` against the existing stored callback variable.

`main/src/adapter/pir/PirAdapter.h` and `main/src/adapter/serial/SerialAdapter.h`: wrap the private statics with the same pattern `Mesh.h` uses:
```cpp
#ifdef UNIT_TEST
public:
#else
private:
#endif
```
(`PirAdapter::instance`, `SerialAdapter::lastHealthMillis`.) clang-format both.

- [ ] **Step 2: Write NodeContext + SimClock**

`tests/e2e/harness/SimClock.h`:
```cpp
#pragma once
#include "time_mock.h"
namespace sim {
class SimClock {
public:
  void advance(uint32_t ms) { _mockMillis += ms; }
  uint32_t now() const { return _mockMillis; }
};
} // namespace sim
```

`tests/e2e/harness/NodeContext.h`:
```cpp
#pragma once
#include <array>
#include <deque>
#include <string>
#include <vector>
#include <cstdint>
#include "EEPROM.h"
#include "serial_mock.h"
#include "esp_now_mock.h"

namespace lattice { namespace mesh { class Mesh; } }
namespace lattice { namespace adapter { class PirAdapter; } }

namespace sim {

// One node's snapshot of every mutable global in the mock layer + firmware statics.
// swapIn() loads it into the globals; swapOut() captures the globals back.
struct NodeContext {
  // Mock layer
  std::array<uint8_t, 512> eepromData;
  int eepromCommitCount = 0;
  std::vector<uint8_t> serialWritten;
  std::string serialOutput;
  std::deque<uint8_t> serialRx;
  std::vector<EspNowSend> espNowSent;
  std::vector<esp_now_peer_info_t> espNowPeers;
  EspNowRecvCb espNowRecvCb = nullptr;
  uint8_t mac[6] = {};
  bool espRestartRequested = false;
  // Firmware statics
  lattice::mesh::Mesh* meshInstance = nullptr;
  lattice::adapter::PirAdapter* pirInstance = nullptr;
  uint32_t serialAdapterLastHealthMillis = 0;
  // Singleton object byte-images (EepromManager, ErrorCore hold per-node flags/pointers;
  // copy ctors are deleted so we snapshot raw bytes — states are flat PODs + raw pointers)
  std::vector<uint8_t> eepromManagerImage;
  std::vector<uint8_t> errorCoreImage;

  NodeContext() { eepromData.fill(0xFF); }
};

void swapIn(NodeContext& ctx);
void swapOut(NodeContext& ctx);

} // namespace sim
```

`tests/e2e/harness/NodeContext.cpp`:
```cpp
#include "NodeContext.h"
#include <cstring>
#include "Arduino.h"
#include "esp_wifi_mock.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/pir/PirAdapter.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/persistence/EepromManager.h"
#include "src/error/ErrorCore.h"

namespace sim {

template <typename T>
static void loadImage(T& obj, std::vector<uint8_t>& image) {
  if (image.size() == sizeof(T)) memcpy(reinterpret_cast<void*>(&obj), image.data(), sizeof(T));
}
template <typename T>
static void saveImage(T& obj, std::vector<uint8_t>& image) {
  image.resize(sizeof(T));
  memcpy(image.data(), reinterpret_cast<void*>(&obj), sizeof(T));
}

void swapIn(NodeContext& ctx) {
  memcpy(EEPROM._data.data(), ctx.eepromData.data(), 512);
  EEPROM._commitCount = ctx.eepromCommitCount;
  Serial.written = ctx.serialWritten;
  Serial.output = ctx.serialOutput;
  Serial.rxQueue = ctx.serialRx;
  espNowSentPackets = ctx.espNowSent;
  espNowRegisteredPeers = ctx.espNowPeers;
  setEspNowRecvCb(ctx.espNowRecvCb);
  memcpy(mockDeviceMac, ctx.mac, 6);
  ESP._restartRequested = ctx.espRestartRequested;
  lattice::mesh::Mesh::instance = ctx.meshInstance;
  lattice::adapter::PirAdapter::instance = ctx.pirInstance;
  lattice::adapter::SerialAdapter::lastHealthMillis = ctx.serialAdapterLastHealthMillis;
  loadImage(lattice::utils::EepromManager::getInstance(), ctx.eepromManagerImage);
  loadImage(lattice::utils::ErrorCore::getInstance(), ctx.errorCoreImage);
}

void swapOut(NodeContext& ctx) {
  memcpy(ctx.eepromData.data(), EEPROM._data.data(), 512);
  ctx.eepromCommitCount = EEPROM._commitCount;
  ctx.serialWritten = Serial.written;
  ctx.serialOutput = Serial.output;
  ctx.serialRx = Serial.rxQueue;
  ctx.espNowSent = espNowSentPackets;
  ctx.espNowPeers = espNowRegisteredPeers;
  ctx.espNowRecvCb = getEspNowRecvCb();
  memcpy(ctx.mac, mockDeviceMac, 6);
  ctx.espRestartRequested = ESP._restartRequested;
  ctx.meshInstance = lattice::mesh::Mesh::instance;
  ctx.pirInstance = lattice::adapter::PirAdapter::instance;
  ctx.serialAdapterLastHealthMillis = lattice::adapter::SerialAdapter::lastHealthMillis;
  saveImage(lattice::utils::EepromManager::getInstance(), ctx.eepromManagerImage);
  saveImage(lattice::utils::ErrorCore::getInstance(), ctx.errorCoreImage);
}

} // namespace sim
```
(If `EepromManager`/`ErrorCore` turn out to contain non-trivially-copyable members — e.g. a `String` — replace the byte-image with explicit UNIT_TEST-gated `snapshotForTest`/`restoreForTest` methods copying each field. Check the class definitions first; as of this plan both hold only bools, ints, and raw pointers.)

- [ ] **Step 3: Add e2e target to tests/CMakeLists.txt**

After the unit test macro/registrations:
```cmake
# ---- E2E simulation target — SIMULATE_MODE recompiles all firmware sources ----
add_executable(lattice_e2e
  e2e/scenarios/test_harness_smoke.cpp
  e2e/harness/NodeContext.cpp
  ${FIRMWARE_SOURCES}
)
target_compile_definitions(lattice_e2e PRIVATE SIMULATE_MODE=1)
target_include_directories(lattice_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_link_libraries(lattice_e2e gtest_main gmock mbedcrypto)
gtest_discover_tests(lattice_e2e PROPERTIES LABELS e2e)
```
(Scenario files accumulate in this one target as later tasks add them.)

- [ ] **Step 4: Write failing swap-isolation test**

`tests/e2e/scenarios/test_harness_smoke.cpp`:
```cpp
#include <gtest/gtest.h>
#include "harness/NodeContext.h"
#include "harness/SimClock.h"
#include "EEPROM.h"
#include "esp_wifi_mock.h"

TEST(NodeContextSwap, IsolatesEepromAndMac) {
  sim::NodeContext a, b;
  a.mac[0] = 0xAA;
  b.mac[0] = 0xBB;

  sim::swapIn(a);
  EEPROM.write(0, 0x11);
  EXPECT_EQ(mockDeviceMac[0], 0xAA);
  sim::swapOut(a);

  sim::swapIn(b);
  EXPECT_EQ(EEPROM.read(0), 0xFF) << "node B must not see node A's EEPROM";
  EXPECT_EQ(mockDeviceMac[0], 0xBB);
  EEPROM.write(0, 0x22);
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_EQ(EEPROM.read(0), 0x11);
  sim::swapOut(a);
}

TEST(SimClockTest, AdvancesMillis) {
  sim::SimClock clock;
  uint32_t t0 = clock.now();
  clock.advance(250);
  EXPECT_EQ(clock.now(), t0 + 250);
  EXPECT_EQ(millis(), clock.now());
}
```

- [ ] **Step 5: Build red → green**

Run: `cmake -B tests/build tests/ && cmake --build tests/build --parallel && ctest --test-dir tests/build -R "NodeContextSwap|SimClockTest" --output-on-failure`
Expected: fails to compile before harness files exist; PASS after.

- [ ] **Step 6: Commit**

```bash
git add tests/ main/src/adapter/
git commit -m "test(e2e): SimClock + NodeContext global-swap harness"
```

---

### Task 4: SimNode — boot and tick a real firmware node

**Files:**
- Create: `tests/e2e/harness/SimNode.h`, `tests/e2e/harness/SimNode.cpp`
- Modify: `tests/e2e/scenarios/test_harness_smoke.cpp` (add tests)
- Modify: `tests/CMakeLists.txt` (add SimNode.cpp to lattice_e2e sources)

**Interfaces:**
- Consumes: `sim::NodeContext`, `swapIn/swapOut`, real `Mesh`, `AdapterFactory`, `EepromManager`.
- Produces:
```cpp
namespace sim {
struct NodeConfig {
  uint8_t mac[6];
  bool isMaster;
  lattice::adapter::adapter_types adapterType;
};
class SimNode {
public:
  explicit SimNode(const NodeConfig& cfg);
  ~SimNode();
  void boot();     // setup()-equivalent inside own context; seeds EEPROM on first boot
  void tick();     // one loop() iteration inside own context; throws err::FatalError on fatal
  void reboot();   // keeps EEPROM image, reconstructs everything else
  bool restartRequested() const { return ctx_.espRestartRequested; }
  NodeContext& ctx() { return ctx_; }
  const uint8_t* mac() const { return cfg_.mac; }
  // Run fn with this node's globals swapped in (for assertions on mesh state)
  template <typename F> auto with(F fn) {
    swapIn(ctx_);
    auto result = fn(*mesh_, adapter_.get());
    swapOut(ctx_);
    return result;
  }
  bool isEnrolled();               // convenience: with() wrapper
  void simulatePirMotion();        // requires PIR adapter; with() wrapper around PirAdapter::simulateMotion()
private:
  NodeConfig cfg_;
  NodeContext ctx_;
  std::unique_ptr<lattice::mesh::Mesh> mesh_;
  std::unique_ptr<lattice::adapter::Adapter> adapter_;
  std::unique_ptr<lattice::hardware::Led> greenLed_, redLed_;
  uint32_t lastEnrollmentBroadcastMs_ = 0;   // mirrors main.ino function-local static
  bool booted_ = false;
};
} // namespace sim
```

- [ ] **Step 1: Write failing tests**

Append to `test_harness_smoke.cpp`:
```cpp
#include "harness/SimNode.h"
#include "lib/lattice-protocol/c/message_types.h"

static sim::NodeConfig masterCfg() {
  return {{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER};
}

TEST(SimNodeTest, MasterBootsAndBeacons) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  // Master beacon interval is 3000ms — tick across 4s of virtual time
  for (int i = 0; i < 4000; ++i) {
    clock.advance(1);
    master.tick();
  }
  // Beacon(s) must have been captured as broadcast esp_now sends
  bool sawBeacon = false;
  for (const auto& pkt : master.ctx().espNowSent) {
    if (pkt.data.size() == sizeof(mesh_message)) {
      const auto* msg = reinterpret_cast<const mesh_message*>(pkt.data.data());
      if (msg->message_type == MESH_TYPE_MASTER_BEACON) sawBeacon = true;
    }
  }
  EXPECT_TRUE(sawBeacon);
}

TEST(SimNodeTest, RebootPreservesEeprom) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  auto imageBefore = master.ctx().eepromData;
  master.reboot();
  // Master flag survives; keypair survives (same EEPROM bytes at PRIVATE_KEY range)
  EXPECT_TRUE(std::equal(imageBefore.begin() + 417, imageBefore.begin() + 483,
                         master.ctx().eepromData.begin() + 417));
  EXPECT_EQ(master.ctx().eepromData[0], imageBefore[0]);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build tests/build --parallel 2>&1 | tail -5`
Expected: compile error (SimNode.h missing).

- [ ] **Step 3: Implement SimNode**

`tests/e2e/harness/SimNode.cpp` — `boot()` mirrors `main/main.ino:83-282` with these deliberate omissions (documented in the header comment): no SevenSegDisplay/DisplayManager, no Buttons/ButtonHandler (both hold function-local statics that can't be swapped; buttons aren't e2e scope), no WDT config (harness has its own budget), no CPU frequency call, no dev-mode branches (nodes run "production" path against seeded EEPROM), no pubkey serial print.

```cpp
#include "SimNode.h"
#include "Arduino.h"
#include "esp_wifi_mock.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/adapter/pir/PirAdapter.h"
#include "src/app/BootManager.h"
#include "src/error/ErrorCore.h"
#include "src/hardware/output/Led.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"
#include "project_config.h"
#include <cstring>

namespace sim {

using lattice::utils::EepromManager;

SimNode::SimNode(const NodeConfig& cfg) : cfg_(cfg) {
  memcpy(ctx_.mac, cfg.mac, 6);
}
SimNode::~SimNode() {
  // Destroy firmware objects while OUR globals are live so any dtor-side
  // effects land in this node's context, then capture.
  swapIn(ctx_);
  adapter_.reset();
  mesh_.reset();
  swapOut(ctx_);
}

void SimNode::boot() {
  swapIn(ctx_);
  Serial.begin(115200);
  lattice::utils::Logger::setLogLevel(lattice::utils::LogLevel::LOG_NONE);

  auto& em = EepromManager::getInstance();
  em.init();
  lattice::app::BootManager::check(em);
  em.setDevMode(false);
  lattice::adapter::AdapterFactory::setDevMode(false);

  greenLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::GREEN_LED_PIN);
  redLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::RED_LED_PIN);
  greenLed_->init();
  redLed_->init();
  lattice::hardware::Led::setSystemErrorLed(redLed_.get());
  lattice::utils::ErrorCore::getInstance().init(redLed_.get(), nullptr);

  if (!booted_) {
    // First boot: seed role + adapter type (a provisioned device's EEPROM)
    em.saveMasterFlag(cfg_.isMaster);
    lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(cfg_.adapterType);
    em.forceFlush();
  }

  lattice::adapter::AdapterFactory::initializeDefaultsIfUnset();
  adapter_.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
  if (!adapter_ || !adapter_->init()) throw lattice::err::FatalError("SimNode: adapter init failed");

  mesh_ = std::make_unique<lattice::mesh::Mesh>();
  if (!mesh_->init()) throw lattice::err::FatalError("SimNode: mesh init failed");
  mesh_->setEnrollmentRelayFn(lattice::adapter::SerialAdapter::relayEnrollmentToServer);
  mesh_->setIsMaster(EepromManager::getInstance().loadMasterFlag());
  adapter_->setTransmitFn(&lattice::mesh::Mesh::transmit);
  mesh_->linkDataRecvCallback([this](const mesh_message& m) {
    if (adapter_) adapter_->onMeshData(m);
  });

  booted_ = true;
  swapOut(ctx_);
}

void SimNode::tick() {
  swapIn(ctx_);
  try {
    lattice::utils::ErrorCore::getInstance().drainPendingBlink();
    mesh_->loop();
    mesh_->checkMasterTimeout();

    // Enrollment state machine (mirrors main.ino loop)
    if (!mesh_->isEnrolled() && !mesh_->getIsMaster()) {
      if (millis() - lastEnrollmentBroadcastMs_ > 10000) {
        lastEnrollmentBroadcastMs_ = millis();
        mesh_->sendEnrollmentRequest();
      }
      swapOut(ctx_);
      return;
    }
    if (adapter_) adapter_->loop();
  } catch (...) {
    swapOut(ctx_);
    throw;
  }
  swapOut(ctx_);
}

void SimNode::reboot() {
  swapIn(ctx_);
  adapter_.reset();
  mesh_.reset();
  lattice::mesh::Mesh::instance = nullptr;
  lattice::adapter::PirAdapter::instance = nullptr;
  ESP._restartRequested = false;
  // EEPROM image survives; everything volatile resets
  Serial.reset();
  resetEspNowMock();
  swapOut(ctx_);
  boot();
}

bool SimNode::isEnrolled() {
  return with([](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.isEnrolled(); });
}

void SimNode::simulatePirMotion() {
  with([](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) {
    auto* pir = dynamic_cast<lattice::adapter::PirAdapter*>(a);
    if (!pir) throw std::runtime_error("simulatePirMotion: node has no PIR adapter");
    pir->simulateMotion();
    return 0;
  });
}

} // namespace sim
```

Implementation notes for the engineer:
- `EepromManager::getInstance()` is process-global; its per-node flags travel via the byte-image in NodeContext. `em.init()` on a swapped-in fresh context must behave like first boot — if it short-circuits on `isInitialized` from a previous node's image, that's exactly what the byte-image swap prevents. If you see cross-node bleed, the image save/restore ordering in swapIn/swapOut is wrong.
- `Mesh::init()` reads `mockDeviceMac` via `esp_wifi_get_mac` — set per node before boot (done via ctx_.mac + swapIn).
- If `Mesh::init()` or EEPROM code paths call `err::fail`, the test-visible effect is `lattice_test_errFailCount` — scenario fixtures assert it stays 0 (Task 7).

- [ ] **Step 4: Build and run**

Run: `cmake --build tests/build --parallel && ctest --test-dir tests/build -R SimNodeTest --output-on-failure`
Expected: both PASS. Debug tips: if `MasterBootsAndBeacons` sees no beacon, check `Mesh::loop()` beacon path requires `isMaster` and peers/broadcast handling in the esp_now mock; dump `master.ctx().espNowSent.size()`.

- [ ] **Step 5: Run full suite + commit**

Run: `ctest --test-dir tests/build --output-on-failure`
```bash
git add tests/
git commit -m "test(e2e): SimNode boots real firmware stack on host"
```

---

### Task 5: VirtualBus — topology-aware frame delivery

**Files:**
- Create: `tests/e2e/harness/VirtualBus.h`, `tests/e2e/harness/VirtualBus.cpp`
- Create: `tests/e2e/harness/SimWorld.h`, `tests/e2e/harness/SimWorld.cpp`
- Modify: `tests/e2e/scenarios/test_harness_smoke.cpp`
- Modify: `tests/CMakeLists.txt` (add new .cpp files to lattice_e2e)

**Interfaces:**
- Consumes: `SimNode::ctx()` (espNowSent, espNowRecvCb), `simulateReceive()` from esp_now mock.
- Produces:
```cpp
namespace sim {
class VirtualBus {
public:
  void addNode(SimNode* n);
  void link(SimNode* a, SimNode* b);     // bidirectional reachability
  void unlink(SimNode* a, SimNode* b);
  bool linked(SimNode* a, SimNode* b) const;
  // Drain every node's captured sends into pending; deliver pending frames
  // (from the PREVIOUS step) into target nodes' recv callbacks.
  void deliver();
  size_t framesInFlight() const;
private:
  struct Pending { SimNode* target; uint8_t src[6]; std::vector<uint8_t> data; };
  std::vector<SimNode*> nodes_;
  std::vector<std::pair<SimNode*, SimNode*>> links_;
  std::vector<Pending> pending_;
  SimNode* findByMac(const uint8_t* mac) const;
};

class SimWorld {
public:
  SimClock clock;
  VirtualBus bus;
  SimNode* addNode(const NodeConfig& cfg);   // owns, boots, registers on bus
  void step(uint32_t ms = 1);   // advance clock, tick all nodes, bus.deliver(), handle restarts
  void run(uint32_t ms);        // step() in a loop
private:
  std::vector<std::unique_ptr<SimNode>> nodes_;
};
} // namespace sim
```

- [ ] **Step 1: Write failing tests**

Append to `test_harness_smoke.cpp`:
```cpp
#include "harness/SimWorld.h"

TEST(VirtualBusTest, BeaconReachesLinkedNodeOnly) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* near = world.addNode({{0x02, 0, 0, 0, 0, 0x02}, false, lattice::adapter::PIR_ADAPTER});
  auto* far = world.addNode({{0x02, 0, 0, 0, 0, 0x03}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, near);
  // 'far' deliberately unlinked

  world.run(4000); // > one beacon interval

  // Linked node has processed a beacon: its mesh learned the master MAC
  bool nearKnowsMaster = near->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  bool farKnowsMaster = far->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  EXPECT_TRUE(nearKnowsMaster);
  EXPECT_FALSE(farKnowsMaster);
}
```
Note: `currentMaster` is a `MasterInfo` (defined in `PeerRegistry.h`) — check its actual field name for the MAC (`mac` vs `macAddress`) and distance before writing; adjust the test to the real field.

- [ ] **Step 2: Verify failure**

Run: `cmake --build tests/build --parallel 2>&1 | tail -5` — compile error (SimWorld missing).

- [ ] **Step 3: Implement**

`VirtualBus.cpp` core:
```cpp
void VirtualBus::deliver() {
  // Phase 1: deliver frames captured on the previous step
  auto delivering = std::move(pending_);
  pending_.clear();
  for (auto& f : delivering) {
    NodeContext& ctx = f.target->ctx();
    if (!ctx.espNowRecvCb) continue;
    swapIn(ctx);
    simulateReceive(f.src, f.data.data(), static_cast<int>(f.data.size()));
    swapOut(ctx);
  }
  // Phase 2: collect this step's sends into pending
  for (SimNode* sender : nodes_) {
    auto enqueue = [&](SimNode* target, const std::vector<uint8_t>& data) {
      Pending p{};
      p.target = target;
      memcpy(p.src, sender->mac(), 6);
      p.data = data;
      pending_.push_back(std::move(p));
    };
    auto& sent = sender->ctx().espNowSent;
    for (auto& pkt : sent) {
      if (pkt.isBroadcast) {
        for (SimNode* other : nodes_)
          if (other != sender && linked(sender, other)) enqueue(other, pkt.data);
      } else {
        SimNode* target = findByMac(pkt.addr);
        // Frame to a MAC no node owns = routing corruption — fail loudly
        if (!target) throw std::runtime_error("VirtualBus: frame to unknown MAC");
        if (linked(sender, target)) enqueue(target, pkt.data);
        // unlinked unicast: silently lost, like RF out of range
      }
    }
    sent.clear();
  }
}
```

`SimWorld.cpp`:
```cpp
SimNode* SimWorld::addNode(const NodeConfig& cfg) {
  nodes_.push_back(std::make_unique<SimNode>(cfg));
  SimNode* n = nodes_.back().get();
  n->boot();
  bus.addNode(n);
  return n;
}

void SimWorld::step(uint32_t ms) {
  clock.advance(ms);
  for (auto& n : nodes_) {
    n->tick();
    if (n->restartRequested()) n->reboot();  // OP_CONFIG_SET hotswap path
  }
  bus.deliver();
}

void SimWorld::run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; ++i) step(1);
}
```
Note on pacing: `run(ms)` at 1ms per step is the deterministic default. If e2e runtime becomes a problem (>30s locally), coarsen to 5ms steps in `run()` — every firmware timing constant (beacon 3000ms, stale 9000ms, enrollment 10000ms) is ≫5ms.

- [ ] **Step 4: Green + full suite**

Run: `cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/
git commit -m "test(e2e): VirtualBus topology delivery + SimWorld scheduler"
```

---

### Task 6: FakeHub — scripted server on the master's serial port

**Files:**
- Create: `tests/e2e/harness/FakeHub.h`, `tests/e2e/harness/FakeHub.cpp`
- Modify: `tests/e2e/scenarios/test_harness_smoke.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: master `SimNode::ctx()` serial buffers; `SerialFraming::encode/decode` (static); `lattice::mesh::crypto::generateKeypair` (MeshCrypto.h, real mbedtls from Task 1).
- Produces:
```cpp
namespace sim {
class FakeHub {
public:
  explicit FakeHub(SimNode* master);
  void poll();  // decode master's Serial.written into received; clears the buffer
  std::vector<mesh_message> received;

  void sendFrame(const mesh_message& msg);              // encode + injectRx to master
  // Enrollment: JOIN_ACK addressed to nodeMac; enrollment_public_key = node's key
  // (echoed back so master registers it); data[0..3] = key fingerprint.
  void approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32);
  void sendConfigSet(const uint8_t* targetMac, lattice::adapter::adapter_types newType);
  void sendHealthReq();

  // Query helpers
  std::vector<mesh_message> ofType(MeshMessageType t) const;
  const mesh_message* enrollmentFrom(const uint8_t* mac) const;  // nullptr if none
  std::vector<mesh_message> adapterDataFromOrigin(const uint8_t* mac) const;
private:
  SimNode* master_;
  std::vector<uint8_t> rxBuffer_;  // accumulates partial frames across poll()s
};
} // namespace sim
```

- [ ] **Step 1: Write failing test**

```cpp
#include "harness/FakeHub.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST(FakeHubTest, ReceivesHealthReportAndAnswersHealthReq) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  sim::FakeHub hub(master);

  world.run(50); // SerialAdapter::loop sends an immediate health report on hop-count init
  hub.poll();
  auto reports = hub.ofType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_FALSE(reports.empty());
  EXPECT_EQ(reports[0].data[0], OP_HEALTH_REPORT);
  EXPECT_EQ(memcmp(&reports[0].data[2], master->mac(), 6), 0);

  size_t before = hub.received.size();
  hub.sendHealthReq();
  world.run(50);
  hub.poll();
  EXPECT_GT(hub.received.size(), before) << "master must answer OP_HEALTH_REQ";
}
```
Note: verify how `SerialAdapter::onMeshDataImpl` vs `sendHealthReport()` actually emit — `sendHealthReport` goes through `Mesh::transmit` (mesh path). On a master node, `transmitCore` routes master-originated SERIAL_ADAPTER data back to its own adapter → serial out. If the report instead appears as a raw mesh send, adjust the assertion to poll after the master's own `onMeshData` processed it. The test as written documents intended behavior; reconcile against real `Mesh::transmitCore` when it fails.

- [ ] **Step 2: Verify failure** — build breaks on missing FakeHub.h.

- [ ] **Step 3: Implement**

`FakeHub.cpp` essentials:
```cpp
void FakeHub::poll() {
  auto& written = master_->ctx().serialWritten;
  rxBuffer_.insert(rxBuffer_.end(), written.begin(), written.end());
  written.clear();
  size_t off = 0;
  while (rxBuffer_.size() - off >= 2) {
    uint16_t len = rxBuffer_[off] | (rxBuffer_[off + 1] << 8);
    if (rxBuffer_.size() - off - 2 < len) break;  // partial frame — wait
    mesh_message msg{};
    if (lattice::adapter::serial::SerialFraming::decode(rxBuffer_.data() + off + 2, len, msg))
      received.push_back(msg);
    off += 2 + len;
  }
  rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + off);
}

void FakeHub::sendFrame(const mesh_message& msg) {
  uint8_t encoded[256];
  size_t n = lattice::adapter::serial::SerialFraming::encode(msg, encoded, sizeof(encoded));
  ASSERT_GT_OR_THROW(n);  // use: if (n == 0) throw std::runtime_error("FakeHub: encode failed");
  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  auto& rx = master_->ctx().serialRx;
  rx.insert(rx.end(), lenLE, lenLE + 2);
  rx.insert(rx.end(), encoded, encoded + n);
}

void FakeHub::approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32) {
  mesh_message ack{};
  ack.proto_version = 2;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.target_mac_address, nodeMac, 6);
  memcpy(ack.enrollment_public_key, nodePubKey32, 32);
  memcpy(ack.data, nodePubKey32, 4);  // fingerprint checked by Enrollment::processJoinAck
  sendFrame(ack);
}

void FakeHub::sendConfigSet(const uint8_t* targetMac, lattice::adapter::adapter_types newType) {
  mesh_message msg{};
  msg.proto_version = 2;
  msg.message_type = MESH_TYPE_SERIAL_CMD_BROADCAST;
  msg.data_type = lattice::adapter::SERIAL_ADAPTER;
  memcpy(msg.target_mac_address, targetMac, 6);
  msg.data[0] = OP_CONFIG_SET;
  memcpy(&msg.data[1], targetMac, 6);
  msg.data[7] = lattice::adapter::AdapterFactory::adapterTypeToEEPROM(newType);
  sendFrame(msg);
}
```
`sendHealthReq()`: same shape, `data[0] = OP_HEALTH_REQ`, broadcast target (all `0xFF`).

- [ ] **Step 4: Green + full suite; commit**

```bash
git add tests/
git commit -m "test(e2e): FakeHub scripted serial server"
```

---

### Task 7: MeshSimTest fixture + enrollment scenario

**Files:**
- Create: `tests/e2e/harness/MeshSimTest.h`
- Create: `tests/e2e/scenarios/test_enrollment_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces the shared fixture all scenarios use:
```cpp
#pragma once
#include <gtest/gtest.h>
#include "harness/SimWorld.h"
#include "harness/FakeHub.h"
#include "src/error/Error.h"

class MeshSimTest : public ::testing::Test {
protected:
  sim::SimWorld world;
  std::unique_ptr<sim::FakeHub> hub;
  sim::SimNode* master = nullptr;

  static constexpr uint8_t MAC_MASTER[6] = {0x02, 0, 0, 0, 0, 0x01};
  static constexpr uint8_t MAC_NODE_A[6] = {0x02, 0, 0, 0, 0, 0x0A};
  static constexpr uint8_t MAC_NODE_B[6] = {0x02, 0, 0, 0, 0, 0x0B};

  void SetUp() override {
    lattice_test_errFailCount = 0;
    _mockMillis = 0;
  }
  void TearDown() override {
    EXPECT_EQ(lattice_test_errFailCount, 0)
        << "a node hit err::fail during the scenario";
  }

  sim::SimNode* addMaster() {
    sim::NodeConfig cfg{};
    memcpy(cfg.mac, MAC_MASTER, 6);
    cfg.isMaster = true;
    cfg.adapterType = lattice::adapter::SERIAL_ADAPTER;
    master = world.addNode(cfg);
    hub = std::make_unique<sim::FakeHub>(master);
    return master;
  }
  sim::SimNode* addSensor(const uint8_t mac[6]) {
    sim::NodeConfig cfg{};
    memcpy(cfg.mac, mac, 6);
    cfg.isMaster = false;
    cfg.adapterType = lattice::adapter::PIR_ADAPTER;
    return world.addNode(cfg);
  }
  // Run world + poll hub every virtual 100ms so partial frames assemble
  void runPolled(uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 100) {
      world.run(std::min<uint32_t>(100, ms - done));
      if (hub) hub->poll();
    }
  }
  // Full happy-path enrollment of `node`; asserts success
  void enroll(sim::SimNode* node) {
    runPolled(11000);
    const mesh_message* req = hub->enrollmentFrom(node->mac());
    ASSERT_NE(req, nullptr) << "enrollment request never reached hub";
    hub->approveEnrollment(node->mac(), req->enrollment_public_key);
    runPolled(5000);
    ASSERT_TRUE(node->isEnrolled());
  }
};
```
(Scenario tests that need err::fail — e.g. serial robustness — reset `lattice_test_errFailCount` before the final assert or override TearDown expectations locally.)

- [ ] **Step 1: Write the enrollment scenario (failing until fixture exists)**

`tests/e2e/scenarios/test_enrollment_e2e.cpp`:
```cpp
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, NodeEnrollsThroughMasterAndHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);

  EXPECT_FALSE(sensor->isEnrolled());
  enroll(sensor);  // asserts request reached hub + JOIN_ACK enrolled the node
}

TEST_F(MeshSimTest, UnenrolledNodeDataNeverReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);

  // No JOIN_ACK issued. PIR data must not appear at the hub.
  runPolled(11000);
  // Node is un-enrolled: its tick() returns before adapter->loop(); motion can't send.
  EXPECT_FALSE(sensor->isEnrolled());
  EXPECT_TRUE(hub->adapterDataFromOrigin(sensor->mac()).empty());
}

TEST_F(MeshSimTest, EnrollmentSurvivesReboot) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->reboot();
  EXPECT_TRUE(sensor->isEnrolled()) << "enrolled flag must persist in EEPROM";
}
```

- [ ] **Step 2: Run red, then debug to green**

Run: `cmake --build tests/build --parallel && ctest --test-dir tests/build -R MeshSimTest --output-on-failure`

This is the first full-stack scenario; expect integration bugs in the harness (not firmware). Debug order when `enrollmentFrom` returns nullptr:
1. `sensor->ctx().espNowSent` after 10s — did the node broadcast MESH_TYPE_ENROLLMENT?
2. Master's recv queue — did VirtualBus deliver it (check `hub->received` vs master `ctx().serialWritten` raw bytes)?
3. Master relays enrollment via `Enrollment::drainPendingRelay()` inside `Mesh::loop()` → `SerialAdapter::relayEnrollmentToServer` writes to Serial — confirm `mesh_->setEnrollmentRelayFn` was wired in SimNode::boot.
4. JOIN_ACK path: hub frame → master serialRx → `SerialAdapter::loop()` reads → `handleCompleteFrame` → `Mesh::enrollPeer` → mesh JOIN_ACK send to node (check `master->ctx().espNowSent`) → node `Enrollment::processJoinAck` fingerprint check (data[0..3] must equal node pubkey[0..3]).

- [ ] **Step 3: Commit**

```bash
git add tests/
git commit -m "test(e2e): MeshSimTest fixture + enrollment scenarios"
```

---

### Task 8: PIR data flow scenario

**Files:**
- Create: `tests/e2e/scenarios/test_pir_dataflow_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:** consumes fixture (`MeshSimTest`), `SimNode::simulatePirMotion()`, `FakeHub::adapterDataFromOrigin`.

- [ ] **Step 1: Write test**

```cpp
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST_F(MeshSimTest, PirMotionReachesHubWithCorrectOriginAndType) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();
  sensor->simulatePirMotion();
  runPolled(2000);

  auto frames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GT(frames.size(), before);
  const mesh_message& f = frames.back();
  EXPECT_EQ(f.data_type, lattice::adapter::PIR_ADAPTER);
  EXPECT_EQ(memcmp(f.origin_mac_address, sensor->mac(), 6), 0);
  EXPECT_EQ(f.hop_count, 1) << "one hop: sensor -> master";
}

TEST_F(MeshSimTest, PirCooldownSuppressesRapidRetrigger) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  runPolled(500);
  size_t afterFirst = hub->adapterDataFromOrigin(sensor->mac()).size();
  sensor->simulatePirMotion();  // within cooldown window
  runPolled(500);
  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), afterFirst)
      << "PIR cooldown must suppress the second trigger";
}
```
Check `PirAdapter.cpp` for the actual cooldown default (`_cooldownSeconds`) — if default cooldown < 1s, widen/narrow the windows so the second trigger genuinely falls inside cooldown.

- [ ] **Step 2: Red → green** (same build/ctest commands; filter `-R Pir`).
- [ ] **Step 3: Commit** — `git commit -m "test(e2e): PIR data flow scenarios"`

---

### Task 9: Multi-hop relay scenario

**Files:**
- Create: `tests/e2e/scenarios/test_multihop_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write test**

```cpp
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, SensorOutOfMasterRangeRelaysThroughMiddleNode) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  // Chain topology: leaf <-> relay <-> master ; leaf CANNOT hear master
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);

  enroll(relay);
  enroll(leaf);   // leaf's enrollment itself must relay through 'relay'

  size_t before = hub->adapterDataFromOrigin(leaf->mac()).size();
  leaf->simulatePirMotion();
  runPolled(3000);

  auto frames = hub->adapterDataFromOrigin(leaf->mac());
  ASSERT_GT(frames.size(), before);
  const mesh_message& f = frames.back();
  EXPECT_EQ(f.hop_count, 2) << "leaf -> relay -> master";
  EXPECT_EQ(memcmp(f.origin_mac_address, leaf->mac(), 6), 0);
  EXPECT_EQ(memcmp(f.last_hop_mac_address, relay->mac(), 6), 0)
      << "last hop must be the relay node";
}

TEST_F(MeshSimTest, NoDuplicateDeliveryInTriangleTopology) {
  addMaster();
  auto* a = addSensor(MAC_NODE_A);
  auto* b = addSensor(MAC_NODE_B);
  // Triangle: everyone hears everyone — replay/dedup must prevent duplicates
  world.bus.link(master, a);
  world.bus.link(master, b);
  world.bus.link(a, b);
  enroll(a);
  enroll(b);

  size_t before = hub->adapterDataFromOrigin(a->mac()).size();
  a->simulatePirMotion();
  runPolled(3000);
  auto frames = hub->adapterDataFromOrigin(a->mac());
  EXPECT_EQ(frames.size(), before + 1)
      << "exactly one copy must reach the hub (replay cache dedups the b-relayed copy)";
}
```
Note: relay jitter (`RELAY_JITTER_MAX_MS = 64`) uses `esp_random()` mock (fixed 42) — deterministic. If enrollment of `leaf` fails: JOIN_ACK downlink relay through `relay` node is the code path under test (`Mesh::processJoinAck` / `relayDownlink`); debug with the Task 7 checklist.

- [ ] **Step 2: Red → green.**
- [ ] **Step 3: Commit** — `git commit -m "test(e2e): multi-hop relay scenarios"`

---

### Task 10: Route report scenario

**Files:**
- Create: `tests/e2e/scenarios/test_route_report_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write test**

```cpp
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST_F(MeshSimTest, RouteReportCarriesHopChain) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);
  enroll(relay);
  enroll(leaf);

  // ROUTE_REPORT_INTERVAL_MS = 2 * HEALTH_REPORT_INTERVAL_MS (see project_config.h)
  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool sawLeafRoute = false;
  for (const auto& m : hub->ofType(MESH_TYPE_ROUTE_REPORT)) {
    if (memcmp(m.origin_mac_address, leaf->mac(), 6) != 0) continue;
    ASSERT_EQ(m.data[0], OP_ROUTE_REPORT);
    uint8_t pathLen = m.data[1];
    ASSERT_GE(pathLen, 1);
    // First hop MAC in the chain must be the relay node
    EXPECT_EQ(memcmp(&m.data[2], relay->mac(), 6), 0);
    sawLeafRoute = true;
  }
  EXPECT_TRUE(sawLeafRoute) << "leaf's route report must reach hub";
}
```
Reconcile payload layout against `Mesh::sendRouteReport()`/`processRouteReport()` (`[B3][1B path_len][path_len × 6B MACs]` per opcodes.h) — adjust index math if the implementation differs, and if it differs from opcodes.h, flag to user (protocol divergence).

- [ ] **Step 2: Red → green.**
- [ ] **Step 3: Commit** — `git commit -m "test(e2e): route report hop chain scenario"`

---

### Task 11: Replay protection scenario

**Files:**
- Create: `tests/e2e/scenarios/test_replay_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write test**

```cpp
#include "harness/MeshSimTest.h"
#include <cstring>

TEST_F(MeshSimTest, ReplayedFrameIsDropped) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  // Capture the raw frame the sensor sends BEFORE the bus consumes it
  world.step(1);  // tick: motion send lands in ctx().espNowSent... (may need a few steps)
  std::vector<uint8_t> captured;
  for (int i = 0; i < 200 && captured.empty(); ++i) {
    for (const auto& pkt : sensor->ctx().espNowSent)
      if (pkt.data.size() == sizeof(mesh_message)) {
        const auto* m = reinterpret_cast<const mesh_message*>(pkt.data.data());
        if (m->message_type == MESH_TYPE_ADAPTER_DATA) captured = pkt.data;
      }
    world.step(1);
  }
  ASSERT_FALSE(captured.empty());

  runPolled(2000);
  size_t legit = hub->adapterDataFromOrigin(sensor->mac()).size();
  ASSERT_GE(legit, 1u);

  // Attacker replays the captured frame at the master
  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), captured.data(), static_cast<int>(captured.size()));
  sim::swapOut(master->ctx());
  runPolled(2000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), legit)
      << "replayed (epoch,seq) must be dropped by ReplayCache";
}

TEST_F(MeshSimTest, RebootBumpsEpochSoNewTrafficIsAccepted) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  runPolled(2000);
  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();
  ASSERT_GE(before, 1u);

  sensor->reboot();  // epoch increments (BOOT_EPOCH in EEPROM); seq resets
  runPolled(2000);   // allow re-sync
  sensor->simulatePirMotion();
  runPolled(3000);

  EXPECT_GT(hub->adapterDataFromOrigin(sensor->mac()).size(), before)
      << "post-reboot traffic (higher epoch, low seq) must be accepted";
}
```

- [ ] **Step 2: Red → green.** If the replayed frame is NOT dropped, inspect `ReplayCache.h` acceptance rules with the actual (epoch, seq) pair — the test harness may have captured a frame the cache never saw (e.g. the relayed copy). Assert against the exact frame that reached the master the first time.
- [ ] **Step 3: Commit** — `git commit -m "test(e2e): replay protection scenarios"`

---

### Task 12: Dual-master failover scenario

**Files:**
- Create: `tests/e2e/scenarios/test_dual_master_e2e.cpp`
- Modify: `tests/CMakeLists.txt`
- Possibly modify: `tests/e2e/harness/SimNode.h/.cpp` — add `void setOffline(bool)` on SimNode (skips tick + bus unlinks) OR use `world.bus.unlink()` to sever the primary.

- [ ] **Step 1: Read the dual-master implementation first**

Read `Mesh::processMasterBeacon`, `checkMasterTimeout`, `Enrollment::knownMasterMacSecondary` usage, and `docs/superpowers/plans/nodes-phase9-dual-master.md`. Determine: how a node learns the secondary master (JOIN_ACK? beacon TOFU? `setDualMasterMode`?), and what "failover" observably does (currentMaster switches to secondary after STALE_MASTER_THRESHOLD_MS = 9000ms). Write the test against those actual mechanics; the skeleton below assumes beacon-TOFU of both masters.

- [ ] **Step 2: Write test**

```cpp
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, NodeFailsOverToSecondaryMasterWhenPrimaryGoesSilent) {
  addMaster();  // primary, MAC_MASTER
  sim::NodeConfig m2cfg{};
  static constexpr uint8_t MAC_MASTER2[6] = {0x02, 0, 0, 0, 0, 0x02};
  memcpy(m2cfg.mac, MAC_MASTER2, 6);
  m2cfg.isMaster = true;
  m2cfg.adapterType = lattice::adapter::SERIAL_ADAPTER;
  auto* master2 = world.addNode(m2cfg);

  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  world.bus.link(master2, sensor);
  world.bus.link(master, master2);
  // Enable dual-master mode on all nodes (mirror however production enables it)
  for (auto* n : {master, master2, sensor})
    n->with([](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
      m.setDualMasterMode(true);
      return 0;
    });
  enroll(sensor);

  // Sensor currently routes to primary
  bool onPrimary = sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  EXPECT_TRUE(onPrimary);

  // Primary goes silent
  world.bus.unlink(master, sensor);
  world.bus.unlink(master, master2);
  runPolled(lattice::config::STALE_MASTER_THRESHOLD_MS + 5000);

  bool onSecondary = sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master2->mac(), 6) == 0;
  });
  EXPECT_TRUE(onSecondary) << "after stale threshold, node must adopt secondary master";
}
```
(Adjust `currentMaster.mac` field name per PeerRegistry.h, as in Task 5.)

- [ ] **Step 3: Red → green.**
- [ ] **Step 4: Commit** — `git commit -m "test(e2e): dual-master failover scenario"`

---

### Task 13: Adapter hotswap scenario

**Files:**
- Create: `tests/e2e/scenarios/test_hotswap_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write test**

```cpp
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, ConfigSetSwapsAdapterTypeAndSurvivesReboot) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);  // boots as PIR
  world.bus.link(master, sensor);
  enroll(sensor);

  auto typeOf = [](sim::SimNode* n) {
    return n->with([](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) {
      return a->getAdapterType();
    });
  };
  ASSERT_EQ(typeOf(sensor), lattice::adapter::PIR_ADAPTER);

  hub->sendConfigSet(sensor->mac(), lattice::adapter::LED_ADAPTER);
  runPolled(5000);  // command → mesh broadcast → node saves EEPROM → ESP.restart → SimWorld reboots it

  EXPECT_EQ(typeOf(sensor), lattice::adapter::LED_ADAPTER)
      << "node must run the new adapter after config-set reboot";

  sensor->reboot();  // plain power cycle
  EXPECT_EQ(typeOf(sensor), lattice::adapter::LED_ADAPTER) << "type persisted in EEPROM";
}

TEST_F(MeshSimTest, ConfigSetForOtherNodeIsIgnored) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  hub->sendConfigSet(MAC_NODE_B, lattice::adapter::LED_ADAPTER);  // different target
  runPolled(5000);

  auto t = sensor->with([](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) {
    return a->getAdapterType();
  });
  EXPECT_EQ(t, lattice::adapter::PIR_ADAPTER);
}
```
Note: OP_CONFIG_SET for non-serial nodes is handled in `Adapter::onMeshData` base class (see Adapter.h comment). Verify what the base class does after saving — if it also calls `ESP.restart()`, SimWorld::step's restart handling covers it; if it soft-switches, drop the reboot expectation and assert the soft switch. Also confirm `LED_ADAPTER` is constructible by `AdapterFactory::createAdapter` on host (it may need an Led mock pin only — fine); if LED_ADAPTER isn't factory-supported, use `SERIAL_ADAPTER` as the target type instead.

- [ ] **Step 2: Red → green.**
- [ ] **Step 3: Commit** — `git commit -m "test(e2e): adapter hotswap via OP_CONFIG_SET"`

---

### Task 14: PIR health + serial robustness scenarios

**Files:**
- Create: `tests/e2e/scenarios/test_pir_health_e2e.cpp`
- Create: `tests/e2e/scenarios/test_serial_robustness_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: PIR health test**

```cpp
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST_F(MeshSimTest, PirNodeHealthReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  runPolled(lattice::config::HEALTH_REPORT_INTERVAL_MS + 5000);

  bool sawNodeHealth = false;
  for (const auto& m : hub->received) {
    if (m.data[0] == OP_NODE_HEALTH && memcmp(&m.data[2], sensor->mac(), 6) == 0)
      sawNodeHealth = true;
  }
  EXPECT_TRUE(sawNodeHealth) << "PIR node's OP_NODE_HEALTH must reach hub";
}
```

- [ ] **Step 2: Serial robustness test**

```cpp
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, GarbageOnSerialDoesNotBreakSubsequentFrames) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  // Garbage: a plausible-looking length prefix followed by junk payload
  uint8_t junk[34];
  junk[0] = 32; junk[1] = 0;                      // claims 32-byte frame
  for (int i = 2; i < 34; ++i) junk[i] = 0x5A;    // undecodable payload
  auto& rx = master->ctx().serialRx;
  rx.insert(rx.end(), junk, junk + sizeof(junk));

  runPolled(1000);
  // Undecodable frame raises err::fail(...,"Failed to decode protobuf frame") — expected here
  EXPECT_GE(lattice_test_errFailCount, 1);
  lattice_test_errFailCount = 0;  // absolve for TearDown

  // Framing must resynchronize: a valid command afterwards still works
  size_t before = hub->received.size();
  hub->sendHealthReq();
  runPolled(1000);
  EXPECT_GT(hub->received.size(), before)
      << "master must recover and answer commands after garbage input";
}
```

- [ ] **Step 3: Red → green; commit**

```bash
git add tests/
git commit -m "test(e2e): PIR health + serial robustness scenarios"
```

---

### Task 15: CI — manual e2e workflow, exclude e2e from PR runs

**Files:**
- Create: `.github/workflows/e2e-tests.yml`
- Modify: `.github/workflows/unit-tests.yml` (ctest exclude)

- [ ] **Step 1: Exclude e2e label from PR workflow**

In `.github/workflows/unit-tests.yml`, change the "Run tests" step:
```yaml
      - name: Run tests
        run: ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 2: Create manual e2e workflow**

`.github/workflows/e2e-tests.yml`:
```yaml
name: E2E Tests

on:
  workflow_dispatch:

jobs:
  e2e:
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0 # v7.0.0
        with:
          submodules: recursive

      - name: Install CMake
        run: sudo apt-get install -y cmake

      - name: Configure
        run: cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build tests/build --parallel

      - name: Run e2e tests
        run: ctest --test-dir tests/build --output-on-failure --label-regex e2e

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1
        with:
          name: e2e-test-results
          path: tests/build/Testing/
```

- [ ] **Step 3: Verify locally**

```bash
ctest --test-dir tests/build --label-exclude e2e | tail -3   # unit only
ctest --test-dir tests/build --label-regex e2e | tail -3     # e2e only
```
Expected: first run excludes all `lattice_e2e` tests; second runs only them; both PASS.

- [ ] **Step 4: Update README testing section**

Add to `README.md` (or `CONTRIBUTING.md` if it documents tests there): brief paragraph — e2e simulation suite exists under `tests/e2e/`, runs the full mesh on host, executed manually via the "E2E Tests" GitHub Action or locally with the ctest label commands above.

- [ ] **Step 5: Commit**

```bash
git add .github/ README.md
git commit -m "ci: manual-trigger e2e workflow; exclude e2e label from PR runs"
```

---

## Self-Review Notes

- Spec coverage: enrollment (T7), PIR data (T8), multi-hop + no-dup (T9), route report (T10), replay + epoch (T11), dual-master (T12), hotswap + persistence (T13), PIR health + serial robustness (T14), unit-test real-code fix (T1), err hooks / watchdog analogue (T2 hooks; per-tick budget folded into `err::FatalError`-on-hang via test timeouts — ctest default timeouts catch infinite loops), CI manual trigger (T15).
- Known reconciliation points (marked in tasks): `MasterInfo` field names (T5/T12), health-report emission path on master (T6), dual-master learning mechanics (T12), OP_CONFIG_SET base-class behavior (T13), route report payload layout (T10). Each has an explicit "read the real code, adjust assertion" instruction — real firmware behavior wins; protocol divergence gets flagged to the user.
- Types consistent: `NodeConfig`/`NodeContext`/`SimNode::with(fn(Mesh&, Adapter*))` used uniformly from T4 onward; `FakeHub` helpers introduced in T6 and only those names used in T7–T14.
