# Phase 4 — PIR Health Heartbeat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Non-master PIR nodes send a health packet every 30s through the mesh using opcode `0xB2`; the server parses and registers them via `UpdateNode`.

**Architecture:** Two independent changes — firmware adds a periodic `sendNodeHealth()` call in `PIR_Adapter::loop()` following the existing `Serial_Adapter` pattern; server extends `handleSerialData` with a `0xB2` case and updates `ParseHealthReport`/`IsHealthReport` to accept both opcodes.

**Tech Stack:** C++17 (ESP32 firmware, GTest), Go 1.22 (server, `go test ./...`)

## Global Constraints

- `OP_NODE_HEALTH = 0xB2` — exact opcode, firmware and server
- Data layout identical to `0xB1`: `[0]=opcode [1]=adapterType(int8) [2..7]=MAC(6B) [8..11]=uptime_sec(uint32 LE)`
- `dataType` in the mesh message header MUST be `SERIAL_ADAPTER` / `AdapterTypeSerial` — same as 0xB1
- Health interval: `planetopia::config::HEALTH_REPORT_INTERVAL_MS` (30000 ms) — do not inline the number
- `adapterTypeToEEPROM(adapter_types::PIR_ADAPTER)` returns `0` — this is `data[1]` on the wire
- `hopCount` from message header, not from data payload
- `PROTO_VERSION = 2` must remain unchanged — do not bump
- Firmware test framework: GTest in `tests/unit/`; mock MAC via `mockDeviceMac[6]` in `tests/mocks/esp_wifi_mock.h`
- Server module root: `motionSensorServer/server/orchestrator/` — run `go test ./mesh/...` from there
- No `gh` CLI commands; no ClearScore email in commits (git identity: `49689582+superbrobenji@users.noreply.github.com`)
- Repos: `Planetopia-nodes` (firmware) and `motionSensorServer` (server), both on `feat/phase4-pir-health`

---

### Task 1: Firmware — PIR node health heartbeat

**Files:**
- Modify: `main/src/Adapter/PIR_Adapter/PIR_Adapter.h`
- Modify: `main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp`
- Modify: `tests/unit/test_pir_adapter.cpp` (create if it does not exist)

**Interfaces:**
- Consumes: `planetopia::config::HEALTH_REPORT_INTERVAL_MS` from `main/project_config.h:118`
- Consumes: `AdapterFactory::adapterTypeToEEPROM(adapter_types::PIR_ADAPTER)` → returns `static_cast<uint8_t>(0)` (value 0)
- Consumes: `mockDeviceMac[6]` from `tests/mocks/esp_wifi_mock.h` (default `{0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}`)
- Consumes: `advanceMillis(uint32_t)` and `resetMillis()` from `tests/mocks/time_mock.h`
- Consumes: `getLastSentData()` or equivalent from `tests/mocks/esp_now_mock.h` — check existing API
- Produces: `PIR_Adapter::sendNodeHealth()` (private, `static void`) — callable from loop
- Produces: `PIR_Adapter::_lastHealthMillis` (private, `uint32_t`) — persists across loop calls

**Context for implementer:**

`Serial_Adapter` (the pattern to follow) declares `static void sendHealthReport()` and `static uint32_t lastHealthMillis` in the header, then in `loop()`:

```cpp
if (stateChanged || millis() - lastHealthMillis >= planetopia::config::HEALTH_REPORT_INTERVAL_MS) {
    lastHealthMillis = static_cast<uint32_t>(millis());
    sendHealthReport();
}
```

`sendHealthReport` body:
```cpp
static void readOwnMac(uint8_t out[6]) { esp_wifi_get_mac(WIFI_IF_STA, out); }

void Serial_Adapter::sendHealthReport() {
    uint8_t data[64] = {0};
    data[0] = OP_HEALTH_REPORT;                                              // 0xB1
    data[1] = AdapterFactory::adapterTypeToEEPROM(adapter_types::SERIAL_ADAPTER);
    uint8_t mac[6]; readOwnMac(mac); memcpy(&data[2], mac, 6);
    uint32_t uptimeSec = millis() / 1000;
    data[8]  = uptimeSec & 0xFF;
    data[9]  = (uptimeSec >> 8)  & 0xFF;
    data[10] = (uptimeSec >> 16) & 0xFF;
    data[11] = (uptimeSec >> 24) & 0xFF;
    planetopia::mesh::Mesh::transmit(adapter_types::SERIAL_ADAPTER, data);
}
```

`PIR_Adapter` MUST send with `dataType = adapter_types::SERIAL_ADAPTER` (same as Serial), `data[0] = 0xB2`, and `data[1] = AdapterFactory::adapterTypeToEEPROM(adapter_types::PIR_ADAPTER)` (= 0).

`PIR_Adapter` uses `sendDataThroughMesh(adapter_types type, uint8_t data[64])` (instance method) NOT `Mesh::transmit` directly — keep consistent with existing motion send.

The `sendNodeHealth()` helper should be `static` like `sendHealthReport()` to match the existing pattern; `_lastHealthMillis` should be non-static (instance member) since `PIR_Adapter` is not a singleton factory — check how `Serial_Adapter::lastHealthMillis` is declared and match it or use instance member.

**Note on esp_now mock:** the tests in `test_mesh_logic.cpp` use `resetEspNowMock()` and `getLastSentMessage()` (or similar). Grep `tests/mocks/esp_now_mock.h` for the available helpers before writing the test assertion.

- [ ] **Step 1: Check existing test file and esp_now_mock helpers**

  ```bash
  # From Planetopia-nodes root
  ls tests/unit/
  grep -n "getLastSent\|lastSent\|sentMessages\|capturedMessages" tests/mocks/esp_now_mock.h tests/mocks/esp_now_mock.cpp
  ```

  Expected: know the function name to retrieve the last sent ESP-NOW packet.

- [ ] **Step 2: Write the failing test**

  Create `tests/unit/test_pir_adapter.cpp` (or append to existing if it already exists):

  ```cpp
  #include <gtest/gtest.h>
  #include "Adapter/PIR_Adapter/PIR_Adapter.h"
  #include "Adapter/AdapterFactory.h"
  #include "esp_now_mock.h"
  #include "esp_wifi_mock.h"
  #include "time_mock.h"
  #include "EEPROM.h"

  using namespace planetopia::adapter;

  class PIRHealthTest : public ::testing::Test {
  protected:
    void SetUp() override {
      EEPROM.reset();
      resetMillis();
      resetEspNowMock();
    }
  };

  TEST_F(PIRHealthTest, SendsNodeHealthAfter30s) {
    PIR_Adapter pir(2);

    // Wire a no-op transmit so sendDataThroughMesh doesn't crash
    pir.setTransmitFn([](adapter_types t, const uint8_t data[64]) {
      // captured via esp_now_mock or a local lambda capture
    });

    // Advance past 30s threshold
    advanceMillis(30001);
    pir.loop();

    // Retrieve last sent packet — replace getLastSentData() with actual mock helper name
    const auto& sent = getLastSentData();  // adjust to actual mock API
    ASSERT_GE(sent.size(), 12u);
    EXPECT_EQ(sent[0], 0xB2);             // OP_NODE_HEALTH
    EXPECT_EQ(sent[1], 0x00);             // PIR_ADAPTER = 0
    // MAC bytes 2..7 should match mockDeviceMac
    uint8_t expectedMac[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    EXPECT_EQ(memcmp(&sent[2], expectedMac, 6), 0);
    // uptime: 30001ms / 1000 = 30s
    uint32_t uptime = sent[8] | (sent[9]<<8) | (sent[10]<<16) | (sent[11]<<24);
    EXPECT_EQ(uptime, 30u);
  }

  TEST_F(PIRHealthTest, DoesNotSendNodeHealthBefore30s) {
    PIR_Adapter pir(2);
    pir.setTransmitFn([](adapter_types, const uint8_t[64]) {});

    advanceMillis(29999);
    pir.loop();

    // Confirm no health packet was sent (adapt assertion to mock API)
    EXPECT_FALSE(wasHealthPacketSent());  // adjust to actual mock API
  }
  ```

  **Before committing this test:** adjust `getLastSentData()` and `wasHealthPacketSent()` to the actual helper names found in step 1.

- [ ] **Step 3: Run test to verify it fails**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
  cmake -B build -S . && cmake --build build 2>&1 | tail -20
  ./build/tests/unit/test_pir_adapter --gtest_filter="PIRHealthTest.*" 2>&1 | tail -20
  ```

  Expected: compile error or linker error (method not yet defined) or FAIL.

- [ ] **Step 4: Add `_lastHealthMillis` member and `sendNodeHealth()` declaration to header**

  File: `main/src/Adapter/PIR_Adapter/PIR_Adapter.h`

  Add to `private:` block after `bool _initialized;`:

  ```cpp
  uint32_t _lastHealthMillis;
  static void sendNodeHealth();
  ```

  Add to constructor initializer list in `PIR_Adapter.cpp`:
  Change the constructor to include `_lastHealthMillis(0)`:

  ```cpp
  PIR_Adapter::PIR_Adapter(int pin)
      : Adapter(pin), _pir(pin), _cooldownSeconds(3), _lastTrigger(0), _timerActive(false),
        _motionSent(false), _interruptEnabled(false), _initialized(false), _lastHealthMillis(0) {
    _adapterType = adapter_types::PIR_ADAPTER;
  }
  ```

- [ ] **Step 5: Implement `sendNodeHealth()` and add 30s timer to `loop()`**

  File: `main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp`

  Add after the existing includes:
  ```cpp
  #include "src/Adapter/AdapterFactory.h"
  #include <esp_wifi.h>
  ```

  Add before `PIR_Adapter::loop()`:
  ```cpp
  static void readOwnMac(uint8_t out[6]) {
    esp_wifi_get_mac(WIFI_IF_STA, out);
  }

  void PIR_Adapter::sendNodeHealth() {
    uint8_t data[64] = {0};
    data[0] = 0xB2;  // OP_NODE_HEALTH
    data[1] = AdapterFactory::adapterTypeToEEPROM(adapter_types::PIR_ADAPTER);
    uint8_t mac[6];
    readOwnMac(mac);
    memcpy(&data[2], mac, 6);
    uint32_t uptimeSec = millis() / 1000;
    data[8]  = uptimeSec & 0xFF;
    data[9]  = (uptimeSec >> 8)  & 0xFF;
    data[10] = (uptimeSec >> 16) & 0xFF;
    data[11] = (uptimeSec >> 24) & 0xFF;
    if (instance)
      instance->sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, data);
  }
  ```

  In `PIR_Adapter::loop()`, add at the end of the function body (before the closing `}`):
  ```cpp
  if (now - _lastHealthMillis >= planetopia::config::HEALTH_REPORT_INTERVAL_MS) {
    _lastHealthMillis = now;
    sendNodeHealth();
  }
  ```

  Full updated `loop()` for reference:
  ```cpp
  void PIR_Adapter::loop() {
    if (!_initialized)
      return;

    uint32_t now = millis();

    if (_pir.isMotionDetected()) {
      _lastTrigger = now;
      _timerActive = true;
      _motionSent = false;
      _pir.clearMotion();
    }

    if (_timerActive && !_motionSent) {
      Logger::logln("PIR_Adapter", "MOTION DETECTED!", LogLevel::LOG_INFO);
      _motionSent = true;
      uint8_t data[64] = {1};
      PIR_Adapter::sendDataTrampoline(_adapterType, data);
    }

    if (_timerActive && (now - _lastTrigger > (_cooldownSeconds * 1000U))) {
      Logger::logln("PIR_Adapter", "Cooldown ended. Re-arming sensor.", LogLevel::LOG_DEBUG);
      _timerActive = false;
      _motionSent = false;

      if (!_pir.attachInterrupt(PIR_Adapter::detectMotionTrampoline, RISING)) {
        planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                              planetopia::core::ModuleDigit::ADAPTER, 2,
                              "PIR_Adapter: Could not re-attach interrupt (possible hardware error)");
        return;
      }
      _interruptEnabled = true;
    }

    if (now - _lastHealthMillis >= planetopia::config::HEALTH_REPORT_INTERVAL_MS) {
      _lastHealthMillis = now;
      sendNodeHealth();
    }
  }
  ```

- [ ] **Step 6: Run tests and verify they pass**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
  cmake --build build && ./build/tests/unit/test_pir_adapter --gtest_filter="PIRHealthTest.*" -v
  ```

  Expected: all PIRHealthTest cases PASS.

- [ ] **Step 7: Run full firmware test suite**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
  ctest --test-dir build --output-on-failure
  ```

  Expected: all tests pass; no regressions.

- [ ] **Step 8: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes add \
    main/src/Adapter/PIR_Adapter/PIR_Adapter.h \
    main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp \
    tests/unit/test_pir_adapter.cpp
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes commit -m "feat(pir): send node health heartbeat every 30s (0xB2)"
  ```

---

### Task 2: Server — handle `OpNodeHealth` (0xB2)

**Files:**
- Modify: `server/orchestrator/mesh/constants.go`
- Modify: `server/orchestrator/mesh/message_builder.go`
- Modify: `server/orchestrator/mesh/server.go`
- Modify: `server/orchestrator/mesh/mesh_test.go`
- Modify: `server/orchestrator/mesh/server_test.go`

**Interfaces:**
- Consumes (from Task 1): the wire format `[0x B2][adapterType uint8][6B MAC][4B uptime LE]` with `DataType == AdapterTypeSerial`
- Produces: `OpNodeHealth byte = 0xB2` in `constants.go`
- Produces: `ParseHealthReport` accepts both `OpHealthReport(0xB1)` and `OpNodeHealth(0xB2)`
- Produces: `IsHealthReport` returns true for both opcodes
- Produces: `handleSerialData` case `OpNodeHealth` → calls `handleHealthReport` (reuse existing handler)

**Context for implementer:**

Current `ParseHealthReport` in `server/orchestrator/mesh/message_builder.go` (lines ~91-133):
```go
func (mb *MessageBuilder) ParseHealthReport(msg *MeshMessage) (*HealthReport, error) {
    if msg.DataType != AdapterTypeSerial { return nil, ... }
    if len(msg.Data) < 12 { return nil, ... }
    if msg.Data[0] != OpHealthReport { return nil, fmt.Errorf("message is not a health report, opcode: 0x%02x", msg.Data[0]) }
    adapterType := int32(int8(msg.Data[1]))
    mac := make([]byte, MACAddressLength); copy(mac, msg.Data[2:8])
    uptime := binary.LittleEndian.Uint32(msg.Data[8:12])
    return &HealthReport{MAC: mac, AdapterType: adapterType, Uptime: uptime, HopCount: msg.HopCount, OriginMAC: msg.OriginMacAddress}, nil
}

func (mb *MessageBuilder) IsHealthReport(msg *MeshMessage) bool {
    return msg.DataType == AdapterTypeSerial && len(msg.Data) >= 1 && msg.Data[0] == OpHealthReport
}
```

The fix for `ParseHealthReport`: change the opcode guard from `!= OpHealthReport` to check for either opcode:
```go
if msg.Data[0] != OpHealthReport && msg.Data[0] != OpNodeHealth {
    return nil, fmt.Errorf("message is not a health report, opcode: 0x%02x", msg.Data[0])
}
```

`IsHealthReport` similarly:
```go
func (mb *MessageBuilder) IsHealthReport(msg *MeshMessage) bool {
    return msg.DataType == AdapterTypeSerial &&
        len(msg.Data) >= 1 &&
        (msg.Data[0] == OpHealthReport || msg.Data[0] == OpNodeHealth)
}
```

`handleSerialData` switch addition — add case alongside existing `OpHealthReport`:
```go
case OpNodeHealth:
    return ms.handleHealthReport(msg)
```

`logMessageToKafka` at line ~628 calls `IsHealthReport` — after updating `IsHealthReport` it will automatically enrich 0xB2 messages too; no separate change needed there.

- [ ] **Step 1: Write failing tests**

  In `server/orchestrator/mesh/mesh_test.go`, add inside the existing `TestMessageBuilder` func (after the existing `ParseHealthReport` sub-test):

  ```go
  t.Run("ParseHealthReport_AcceptsNodeHealth_0xB2", func(t *testing.T) {
      data := make([]byte, MaxDataLength)
      data[0] = OpNodeHealth  // 0xB2
      data[1] = byte(AdapterTypePIR)
      mac := []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
      copy(data[2:8], mac)
      data[8] = 0x1E // 30 seconds
      data[9] = 0x00
      data[10] = 0x00
      data[11] = 0x00

      msg := &MeshMessage{
          MessageType: MessageTypeAdapterData,
          DataType:    AdapterTypeSerial,
          Data:        data,
          HopCount:    3,
      }

      report, err := builder.ParseHealthReport(msg)
      if err != nil {
          t.Fatalf("Expected no error for 0xB2, got %v", err)
      }
      if !bytes.Equal(report.MAC, mac) {
          t.Errorf("MAC mismatch: got %x", report.MAC)
      }
      if report.AdapterType != AdapterTypePIR {
          t.Errorf("AdapterType: got %d, want %d", report.AdapterType, AdapterTypePIR)
      }
      if report.Uptime != 30 {
          t.Errorf("Uptime: got %d, want 30", report.Uptime)
      }
      if report.HopCount != 3 {
          t.Errorf("HopCount: got %d, want 3", report.HopCount)
      }
  })

  t.Run("IsHealthReport_TrueFor_0xB2", func(t *testing.T) {
      data := make([]byte, MaxDataLength)
      data[0] = OpNodeHealth
      msg := &MeshMessage{DataType: AdapterTypeSerial, Data: data}
      if !builder.IsHealthReport(msg) {
          t.Error("IsHealthReport should return true for 0xB2")
      }
  })
  ```

  In `server/orchestrator/mesh/server_test.go`, add after `TestHandleMessage_ProtoVersionGuard`:

  ```go
  func TestHandleNodeHealth_RegistersNode(t *testing.T) {
      ms := newTestMeshServer(t)
      mac := []byte{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01}

      data := make([]byte, MaxDataLength)
      data[0] = byte(OpNodeHealth) // 0xB2
      data[1] = byte(AdapterTypePIR)
      copy(data[2:8], mac)
      data[8]  = 60  // uptime = 60s
      data[9]  = 0
      data[10] = 0
      data[11] = 0

      msg := &MeshMessage{
          ProtoVersion:     2,
          MessageType:      MessageTypeAdapterData,
          DataType:         AdapterTypeSerial,
          Data:             data,
          OriginMacAddress: mac,
          HopCount:         2,
      }

      if err := ms.handleMessage(msg); err != nil {
          t.Fatalf("handleMessage returned error: %v", err)
      }

      node, ok := ms.GetNodeRegistry().GetNode(mac)
      if !ok {
          t.Fatal("node not registered after 0xB2 health report")
      }
      if node.AdapterType != AdapterTypePIR {
          t.Errorf("AdapterType: got %d, want %d", node.AdapterType, AdapterTypePIR)
      }
      if node.Uptime != 60 {
          t.Errorf("Uptime: got %d, want 60", node.Uptime)
      }
  }
  ```

- [ ] **Step 2: Run tests to verify they fail**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  go test ./mesh/... -run "TestMessageBuilder/ParseHealthReport_AcceptsNodeHealth|TestMessageBuilder/IsHealthReport_TrueFor|TestHandleNodeHealth" -v 2>&1 | tail -20
  ```

  Expected: FAIL — `OpNodeHealth` undefined / opcode guard rejects 0xB2.

- [ ] **Step 3: Add `OpNodeHealth` constant**

  File: `server/orchestrator/mesh/constants.go`

  In the Serial Control Opcodes block, add after `OpHealthReport`:
  ```go
  OpNodeHealth    byte = 0xB2 // Non-serial node → server health status
  ```

- [ ] **Step 4: Update `ParseHealthReport` and `IsHealthReport`**

  File: `server/orchestrator/mesh/message_builder.go`

  Change:
  ```go
  if msg.Data[0] != OpHealthReport {
      return nil, fmt.Errorf("message is not a health report, opcode: 0x%02x", msg.Data[0])
  }
  ```
  To:
  ```go
  if msg.Data[0] != OpHealthReport && msg.Data[0] != OpNodeHealth {
      return nil, fmt.Errorf("message is not a health report, opcode: 0x%02x", msg.Data[0])
  }
  ```

  Change `IsHealthReport`:
  ```go
  func (mb *MessageBuilder) IsHealthReport(msg *MeshMessage) bool {
      return msg.DataType == AdapterTypeSerial &&
          len(msg.Data) >= 1 &&
          (msg.Data[0] == OpHealthReport || msg.Data[0] == OpNodeHealth)
  }
  ```

- [ ] **Step 5: Add `OpNodeHealth` case in `handleSerialData`**

  File: `server/orchestrator/mesh/server.go`

  Change the switch in `handleSerialData` from:
  ```go
  switch opcode {
  case OpHealthReport:
      return ms.handleHealthReport(msg)
  default:
  ```
  To:
  ```go
  switch opcode {
  case OpHealthReport, OpNodeHealth:
      return ms.handleHealthReport(msg)
  default:
  ```

- [ ] **Step 6: Run targeted tests**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  go test ./mesh/... -run "TestMessageBuilder/ParseHealthReport_AcceptsNodeHealth|TestMessageBuilder/IsHealthReport_TrueFor|TestHandleNodeHealth" -v 2>&1 | tail -20
  ```

  Expected: all three sub-tests PASS.

- [ ] **Step 7: Run full server test suite**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  go test ./mesh/... -v 2>&1 | tail -30
  ```

  Expected: all tests pass; no regressions.

- [ ] **Step 8: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/constants.go \
    server/orchestrator/mesh/message_builder.go \
    server/orchestrator/mesh/server.go \
    server/orchestrator/mesh/mesh_test.go \
    server/orchestrator/mesh/server_test.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit -m "feat(server): handle OP_NODE_HEALTH (0xB2) from PIR nodes"
  ```
