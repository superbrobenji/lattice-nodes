# Phase 5 — Node Identity (Logical ID + 7-seg Display) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Server assigns a persistent logical node ID (1–255) at enrollment. The ID is stored in EEPROM and shown on the 7-seg display. Display reflects enrollment state.

**Architecture:** Three sequential tasks — (1) firmware EEPROM field + opcode handler for ID assignment, (2) firmware 7-seg display state machine in main.ino, (3) server NodeInfo extension + approval API + OP_NODE_ID_SET frame send. Tasks 1 and 2 are firmware-only; Task 3 is server-only and can be written independently.

**Tech Stack:** C++17 (ESP32 firmware, GTest), Go 1.22 (server, `go test ./...`)

## Global Constraints

- `OP_NODE_ID_SET = 0xC0` — exact opcode value, firmware and server
- **Wire format deviation from spec:** spec states `[0xC0][1B nodeId]`; actual format is `[0xC0][6B targetMAC][1B nodeId]` (following `OP_CONFIG_SET` pattern — targetMAC allows unicast targeting; use `0xFF×6` for broadcast)
- `NODE_ID` EEPROM address: `496` (1 byte); EEPROM comment update: "Total used: 497 bytes"
- Node IDs 1–255; `0` means "not yet assigned"
- `PROTO_VERSION = 2` unchanged
- Server auto-assigns next free ID when `nodeId == 0` in approval request
- `UpdateNode()` must NOT overwrite NodeID/Name/Zone once assigned (sticky fields)
- Unenrolled display: `setSegments({0x40,0x40,0x40,0x40})` flashing 500ms
- Enrolled no-ID display: `show(0, false)` → `   0`
- Enrolled sensor: `show(nodeId, false)` → right-aligned e.g. `  07`
- Master: `showWithDP(nodeId, false)` → same number + decimal point on last digit
- No `gh` CLI; git identity `49689582+superbrobenji@users.noreply.github.com`
- Firmware repo: `Planetopia-nodes`, branch `feat/phase5-node-identity`
- Server repo: `motionSensorServer`, branch `feat/phase5-node-identity`
- Server module root: `motionSensorServer/server/orchestrator/`

---

### Task 1: Firmware — NODE_ID EEPROM + OP_NODE_ID_SET opcode handler

**Files:**
- Modify: `main/src/persistence/EEPROM_Manager.h` — add `NODE_ID = 496` address, update layout comment, declare `loadNodeId()`/`saveNodeId()`
- Modify: `main/src/persistence/EEPROM_Manager.cpp` — implement `loadNodeId()`/`saveNodeId()`
- Modify: `main/src/Adapter/Adapter.cpp` — handle `OP_NODE_ID_SET` in `onMeshData` base class
- Modify: `main/src/Adapter/Serial_Adapter/Serial_Adapter.h` — add `OP_NODE_ID_SET = 0xC0` constant
- Modify: `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` — handle `OP_NODE_ID_SET` in `handleCompleteFrame` opcode block for master
- Modify: `tests/unit/test_eeprom_manager.cpp` — add NODE_ID tests
- Modify: `tests/unit/test_mesh_logic.cpp` OR create `tests/unit/test_pir_adapter.cpp` addition — OP_NODE_ID_SET opcode dispatch

**Interfaces:**
- Produces: `EEPROM_Manager::loadNodeId() → uint8_t`
- Produces: `EEPROM_Manager::saveNodeId(uint8_t nodeId)`
- Produces: `Serial_Adapter::OP_NODE_ID_SET = 0xC0`

**Context for implementer:**

The EEPROM layout currently ends at 495 (TX_POWER_PRESET). Address 496 is free. The layout comment in `EEPROM_Manager.h` says "Total used: 496 bytes — fits in 512"; after this change it becomes 497 bytes.

`saveNodeId`/`loadNodeId` follow the same pattern as `saveTxPowerPreset`/`loadTxPowerPreset`:
```cpp
void EEPROM_Manager::saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized()) return;
  EEPROM.write(EEPROM_ADDRESSES::NODE_ID, nodeId);
  markDirty();
  logOperation("saveNodeId");
}

uint8_t EEPROM_Manager::loadNodeId() {
  if (!ensureInitialized()) return 0;
  return EEPROM.read(EEPROM_ADDRESSES::NODE_ID);
}
```
A freshly erased EEPROM reads `0xFF` from unwritten bytes. Treat `0xFF` as "unset" → return `0`. Adjust:
```cpp
uint8_t raw = EEPROM.read(EEPROM_ADDRESSES::NODE_ID);
return (raw == 0xFF) ? 0 : raw;
```

**OP_NODE_ID_SET in `Adapter::onMeshData`** (follows `OP_CONFIG_SET` pattern in `Adapter.cpp`):
```cpp
static constexpr uint8_t OP_NODE_ID_SET = 0xC0;  // [C0][6B targetMAC][1B nodeId]
// ... inside the SERIAL_ADAPTER block, alongside OP_CONFIG_SET:
if (op == OP_NODE_ID_SET && len(message.data) >= 8) {
    uint8_t ownMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ownMac);
    bool allFF = true;
    for (int i = 0; i < 6; ++i) {
        if (message.data[1 + i] != 0xFF) { allFF = false; break; }
    }
    bool isTarget = allFF || (memcmp(&message.data[1], ownMac, 6) == 0);
    if (isTarget) {
        uint8_t nodeId = message.data[7];
        planetopia::utils::EEPROM_Manager::getInstance().saveNodeId(nodeId);
        Logger::logln("ADAPTER", "Node ID assigned: " + String(nodeId), LogLevel::LOG_INFO);
    }
}
```
Note: `message.data` is `uint8_t[64]`; no bounds check needed beyond opcode guard since `len >= 8` is guaranteed by `data[64]` size.

**OP_NODE_ID_SET in `Serial_Adapter::handleCompleteFrame`** (for master node, mirrors OP_CONFIG_SET block at line ~447):
```cpp
} else if (op == OP_NODE_ID_SET) {
    if (msg.data[0] == OP_NODE_ID_SET) {  // redundant but explicit
        uint8_t myMac[6];
        readOwnMac(myMac);
        bool allFF = true;
        for (int i = 0; i < 6; ++i)
            if (msg.data[1 + i] != 0xFF) { allFF = false; break; }
        bool isTarget = allFF || (memcmp(&msg.data[1], myMac, 6) == 0);
        if (isTarget) {
            uint8_t nodeId = msg.data[7];
            planetopia::utils::EEPROM_Manager::getInstance().saveNodeId(nodeId);
            Logger::logln("Serial_Adapter", "Node ID set: " + String(nodeId), LogLevel::LOG_INFO);
        }
    }
}
```

**Testing `OP_NODE_ID_SET`:** The opcode flows through mesh (adapter data), which is tested in `test_mesh_logic.cpp`. The EEPROM save is tested in `test_eeprom_manager.cpp`. For the opcode dispatch, write a test in `test_pir_adapter.cpp` that creates a PIR_Adapter, injects a mesh message with `dataType=SERIAL_ADAPTER, data[0]=0xC0, data[1..6]=mockDeviceMac, data[7]=42`, calls `adapter->onMeshData(message)`, then asserts `EEPROM_Manager::getInstance().loadNodeId() == 42`.

- [ ] **Step 1: Add `NODE_ID` address and layout comment update to `EEPROM_Manager.h`**

  Add to the address namespace block after `TX_POWER_PRESET`:
  ```cpp
  constexpr uint16_t NODE_ID = 496; // 1 byte: logical node ID assigned by server (0 = unset)
  ```

  Update the layout comment block:
  ```
  // 495   TX_POWER_PRESET  (1 byte) — TxPowerPreset enum value (0=SHORT_RANGE 1=INDOOR 2=OUTDOOR)
  // 496   NODE_ID          (1 byte) — logical node ID assigned by server (0 = unset, 0xFF = erased)
  // Total used: 497 bytes — fits in 512
  ```

  Add declarations to class body after `saveTxPowerPreset`:
  ```cpp
  uint8_t loadNodeId();
  void saveNodeId(uint8_t nodeId);
  ```

- [ ] **Step 2: Write the failing EEPROM tests**

  In `tests/unit/test_eeprom_manager.cpp`, add:
  ```cpp
  TEST(EEPROM_Manager, NodeId_DefaultIsZero) {
    EEPROM.reset();
    auto& em = planetopia::utils::EEPROM_Manager::getInstance();
    em.init();
    EXPECT_EQ(em.loadNodeId(), 0u);
  }

  TEST(EEPROM_Manager, NodeId_SaveAndLoad) {
    EEPROM.reset();
    auto& em = planetopia::utils::EEPROM_Manager::getInstance();
    em.init();
    em.saveNodeId(42);
    em.forceFlush();
    EXPECT_EQ(em.loadNodeId(), 42u);
  }

  TEST(EEPROM_Manager, NodeId_SaveZeroRoundtrips) {
    EEPROM.reset();
    auto& em = planetopia::utils::EEPROM_Manager::getInstance();
    em.init();
    em.saveNodeId(7);
    em.saveNodeId(0);
    em.forceFlush();
    EXPECT_EQ(em.loadNodeId(), 0u);
  }
  ```

- [ ] **Step 3: Run tests to verify they fail**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
  cmake -B build -S tests && cmake --build build 2>&1 | grep -E "error|FAILED" | head -10
  ctest --test-dir build --output-on-failure -R EEPROM 2>&1 | tail -10
  ```

  Expected: compile error or FAIL (loadNodeId not defined).

- [ ] **Step 4: Implement `loadNodeId` and `saveNodeId` in `EEPROM_Manager.cpp`**

  Add after the `saveTxPowerPreset`/`loadTxPowerPreset` block:
  ```cpp
  void EEPROM_Manager::saveNodeId(uint8_t nodeId) {
    if (!ensureInitialized()) return;
    EEPROM.write(EEPROM_ADDRESSES::NODE_ID, nodeId);
    markDirty();
    logOperation("saveNodeId");
  }

  uint8_t EEPROM_Manager::loadNodeId() {
    if (!ensureInitialized()) return 0;
    uint8_t raw = EEPROM.read(EEPROM_ADDRESSES::NODE_ID);
    return (raw == 0xFF) ? 0 : raw;
  }
  ```

- [ ] **Step 5: Run EEPROM tests to verify they pass**

  ```bash
  cmake --build /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build
  ctest --test-dir /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build --output-on-failure -R EEPROM 2>&1 | tail -10
  ```

  Expected: NodeId_DefaultIsZero, NodeId_SaveAndLoad, NodeId_SaveZeroRoundtrips all PASS.

- [ ] **Step 6: Add `OP_NODE_ID_SET` to `Serial_Adapter.h`**

  In the serial control opcodes block, after `OP_HEALTH_REPORT`:
  ```cpp
  static constexpr uint8_t OP_NODE_ID_SET = 0xC0; // [C0][6B targetMAC][1B nodeId]
  ```

- [ ] **Step 7: Write failing OP_NODE_ID_SET dispatch test**

  In `tests/unit/test_pir_adapter.cpp` (or a new `test_adapter_base.cpp`), add a test class. The simplest approach reuses `PIRHealthTest` fixture or a new GTest fixture:

  ```cpp
  TEST_F(PIRHealthTest, OpNodeIdSet_AssignsNodeId_WhenTargetMatchesMac) {
    PIR_Adapter* pir = new PIR_Adapter(2);
    pir->setTransmitFn([](adapter_types, const uint8_t[64]) {});

    // Set mockDeviceMac to known value
    mockDeviceMac[0] = 0x11; mockDeviceMac[1] = 0x22; mockDeviceMac[2] = 0x33;
    mockDeviceMac[3] = 0x44; mockDeviceMac[4] = 0x55; mockDeviceMac[5] = 0x66;

    planetopia::mesh::mesh_message msg{};
    msg.dataType = adapter_types::SERIAL_ADAPTER;
    msg.data[0] = 0xC0; // OP_NODE_ID_SET
    // target MAC = mockDeviceMac
    memcpy(&msg.data[1], mockDeviceMac, 6);
    msg.data[7] = 99; // nodeId

    pir->onMeshData(msg);

    EXPECT_EQ(planetopia::utils::EEPROM_Manager::getInstance().loadNodeId(), 99u);
    delete pir;
  }

  TEST_F(PIRHealthTest, OpNodeIdSet_IgnoresMessage_WhenTargetMismatch) {
    EEPROM.reset();
    PIR_Adapter* pir = new PIR_Adapter(2);
    pir->setTransmitFn([](adapter_types, const uint8_t[64]) {});

    mockDeviceMac[0] = 0xAA; mockDeviceMac[1] = 0xBB; mockDeviceMac[2] = 0xCC;
    mockDeviceMac[3] = 0xDD; mockDeviceMac[4] = 0xEE; mockDeviceMac[5] = 0xFF;

    planetopia::mesh::mesh_message msg{};
    msg.dataType = adapter_types::SERIAL_ADAPTER;
    msg.data[0] = 0xC0;
    uint8_t differentMac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(&msg.data[1], differentMac, 6);
    msg.data[7] = 55;

    pir->onMeshData(msg);

    EXPECT_EQ(planetopia::utils::EEPROM_Manager::getInstance().loadNodeId(), 0u);
    delete pir;
  }
  ```

- [ ] **Step 8: Run to verify failure**

  ```bash
  cmake --build /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build
  ctest --test-dir /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build --output-on-failure -R PIRHealth 2>&1 | tail -10
  ```

  Expected: OpNodeIdSet tests FAIL (opcode not handled).

- [ ] **Step 9: Implement OP_NODE_ID_SET in `Adapter::onMeshData`**

  File: `main/src/Adapter/Adapter.cpp`

  Inside the `if (message.dataType == adapter_types::SERIAL_ADAPTER)` block, add after the `OP_CONFIG_SET` block and before the `if (_adapterType != adapter_types::SERIAL_ADAPTER) return;` guard:

  ```cpp
  static constexpr uint8_t OP_NODE_ID_SET = 0xC0;
  if (op == OP_NODE_ID_SET) {
    uint8_t ownMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ownMac);
    bool allFF = true;
    for (int i = 0; i < 6; ++i) {
      if (message.data[1 + i] != 0xFF) { allFF = false; break; }
    }
    bool isTarget = allFF || (memcmp(&message.data[1], ownMac, 6) == 0);
    if (isTarget) {
      uint8_t nodeId = message.data[7];
      planetopia::utils::EEPROM_Manager::getInstance().saveNodeId(nodeId);
      Logger::logln("ADAPTER", "Node ID assigned: " + String(nodeId), LogLevel::LOG_INFO);
    }
  }
  ```

- [ ] **Step 10: Implement OP_NODE_ID_SET in `Serial_Adapter::handleCompleteFrame`**

  File: `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp`

  In the control opcode block (after `OP_CONFIG_SET` at line ~447), add:
  ```cpp
  } else if (op == OP_NODE_ID_SET) {
    uint8_t myMac[6];
    readOwnMac(myMac);
    bool allFF = true;
    for (int i = 0; i < 6; ++i)
      if (msg.data[1 + i] != 0xFF) { allFF = false; break; }
    bool isTarget = allFF || (memcmp(&msg.data[1], myMac, 6) == 0);
    if (isTarget) {
      uint8_t nodeId = msg.data[7];
      planetopia::utils::EEPROM_Manager::getInstance().saveNodeId(nodeId);
      Logger::logln("Serial_Adapter", "Node ID set: " + String(nodeId), LogLevel::LOG_INFO);
    }
  }
  ```

- [ ] **Step 11: Run all tests**

  ```bash
  cmake --build /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build
  ctest --test-dir /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build --output-on-failure 2>&1 | tail -10
  ```

  Expected: all tests pass (including new EEPROM + PIRHealth opcode tests).

- [ ] **Step 12: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes add \
    main/src/persistence/EEPROM_Manager.h \
    main/src/persistence/EEPROM_Manager.cpp \
    main/src/Adapter/Adapter.cpp \
    main/src/Adapter/Serial_Adapter/Serial_Adapter.h \
    main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp \
    tests/unit/test_eeprom_manager.cpp \
    tests/unit/test_pir_adapter.cpp
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes commit \
    -m "feat(firmware): NODE_ID EEPROM field + OP_NODE_ID_SET (0xC0) handler"
  ```

---

### Task 2: Firmware — SevenSegDisplay DP + main.ino display states

**Files:**
- Modify: `main/src/hardware/output/SevenSegDisplay.h` — add `showWithDP(int value, bool leadingZeros = false)` declaration
- Modify: `main/src/hardware/output/SevenSegDisplay.cpp` — implement `showWithDP`
- Modify: `main/main.ino` — add display state machine in `loop()`

**Interfaces:**
- Consumes (from Task 1): `EEPROM_Manager::loadNodeId() → uint8_t`
- Consumes: `mesh.isEnrolled() → bool`, `mesh.getIsMaster() → bool`
- Produces: `SevenSegDisplay::showWithDP(int value, bool leadingZeros)`

**Context for implementer:**

`SevenSegDisplay::show(int value, bool leadingZeros)` computes 4 segment bytes and calls `setSegments(segs)`. `showWithDP` is identical but ORs `0x80` (DP bit) into `segs[3]` before `setSegments`. Implement by copying the logic from `show` and adding the OR:

```cpp
void SevenSegDisplay::showWithDP(int value, bool leadingZeros) {
  bool negative = value < 0;
  int v = negative ? -value : value;
  if (v > 9999) v = 9999;

  int digits[4];
  for (int i = 3; i >= 0; --i) { digits[i] = v % 10; v /= 10; }

  if (negative) {
    for (int i = 0; i < 4; ++i) {
      if (digits[i] != 0 || i == 3) { digits[i] = -1; break; }
    }
  }

  uint8_t segs[4];
  for (int i = 0; i < 4; ++i) {
    if (!leadingZeros && digits[i] == 0 && i < 3 && !negative)
      segs[i] = 0x00;
    else
      segs[i] = encodeDigit(digits[i]);
  }
  segs[3] |= 0x80; // decimal point on last digit
  setSegments(segs);
}
```

**Display state machine in `main.ino` `loop()`:**

Add at the START of `loop()` (before any adapter->loop() calls), guarded by `ENABLE_SEVSEG_DISPLAY`:

```cpp
if (planetopia::config::ENABLE_SEVSEG_DISPLAY) {
  static uint32_t lastDisplayToggleMs = 0;
  static bool dashVisible = false;

  bool enrolled = mesh.isEnrolled() || mesh.getIsMaster(); // master is always "enrolled"
  uint8_t nodeId = planetopia::utils::EEPROM_Manager::getInstance().loadNodeId();

  if (!enrolled) {
    // Unenrolled: flash "----" at 500ms
    if (millis() - lastDisplayToggleMs >= 500) {
      lastDisplayToggleMs = static_cast<uint32_t>(millis());
      dashVisible = !dashVisible;
      if (dashVisible) {
        static const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
        sevenSeg.setSegments(dashes);
      } else {
        sevenSeg.clear();
      }
    }
  } else if (nodeId == 0) {
    // Enrolled, no ID: "   0"
    sevenSeg.show(0, false);
  } else if (mesh.getIsMaster()) {
    // Master with ID: right-aligned nodeId + DP on last digit
    sevenSeg.showWithDP(static_cast<int>(nodeId), false);
  } else {
    // Sensor node with ID: right-aligned nodeId
    sevenSeg.show(static_cast<int>(nodeId), false);
  }
}
```

Place this block AFTER the existing `mesh.loop()` call and BEFORE `adapter->loop()`. The display update is cheap and synchronous; 1ms delay at end of loop() is unaffected.

Note on `mesh.isEnrolled()`: this returns false for the master node (master is never "enrolled" in the client sense). Master is always "active" so treat `getIsMaster()` as "enrolled" for display purposes.

**Tests:** `SevenSegDisplay::showWithDP` calls `setSegments` which requires GPIO/hardware. No unit tests exist for SevenSegDisplay (hardware-only class). Verify by:
1. Building the firmware (`cmake --build build`) confirms no compile errors
2. Manual observation on device — document in task report

- [ ] **Step 1: Add `showWithDP` declaration to `SevenSegDisplay.h`**

  After the existing `show` declaration:
  ```cpp
  // Like show() but with a decimal point on the rightmost digit.
  void showWithDP(int value, bool leadingZeros = false);
  ```

- [ ] **Step 2: Implement `showWithDP` in `SevenSegDisplay.cpp`**

  Add after the existing `show` implementation (line ~179):
  ```cpp
  void SevenSegDisplay::showWithDP(int value, bool leadingZeros) {
    bool negative = value < 0;
    int v = negative ? -value : value;
    if (v > 9999) v = 9999;

    int digits[4];
    for (int i = 3; i >= 0; --i) {
      digits[i] = v % 10;
      v /= 10;
    }

    if (negative) {
      for (int i = 0; i < 4; ++i) {
        if (digits[i] != 0 || i == 3) {
          digits[i] = -1;
          break;
        }
      }
    }

    uint8_t segs[4];
    for (int i = 0; i < 4; ++i) {
      if (!leadingZeros && digits[i] == 0 && i < 3 && !negative)
        segs[i] = 0x00;
      else
        segs[i] = encodeDigit(digits[i]);
    }
    segs[3] |= 0x80; // DP bit on last digit
    setSegments(segs);
  }
  ```

- [ ] **Step 3: Add display state machine to `main.ino`**

  In `loop()`, add the display block shown in Context above. Place it after `mesh.loop()` and before `adapter->loop()`.

- [ ] **Step 4: Build to verify no compile errors**

  ```bash
  cmake --build /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build 2>&1 | grep -E "error:|warning:" | head -20
  ```

  Expected: clean build, no errors or new warnings.

- [ ] **Step 5: Run full test suite**

  ```bash
  ctest --test-dir /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/build --output-on-failure 2>&1 | tail -10
  ```

  Expected: all tests pass (display code is not exercised by unit tests but compile correctness is verified).

- [ ] **Step 6: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes add \
    main/src/hardware/output/SevenSegDisplay.h \
    main/src/hardware/output/SevenSegDisplay.cpp \
    main/main.ino
  git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes commit \
    -m "feat(display): 7-seg display states + showWithDP for master node"
  ```

---

### Task 3: Server — NodeInfo identity fields + ApproveEnrollment + OP_NODE_ID_SET send

**Files:**
- Modify: `server/orchestrator/mesh/constants.go` — add `OpNodeIdSet byte = 0xC0`
- Modify: `server/orchestrator/mesh/node_registry.go` — add `NodeID`, `Name`, `Zone` to `NodeInfo` and `persistedNode`; add `AssignNode`, `NextFreeNodeID` methods; update `UpdateNode` (don't overwrite sticky fields); update `Persist`/`Load`
- Modify: `server/orchestrator/mesh/server.go` — change `ApproveEnrollment` to accept `ApprovalParams`; after JOIN_ACK send `OP_NODE_ID_SET` frame; call `AssignNode`
- Modify: `server/orchestrator/mesh/api.go` — add `ApprovalRequest` struct; update `approveEnrollment` to parse body
- Modify: `server/orchestrator/mesh/server_enrollment_test.go` — update existing `ApproveEnrollment` callers for new signature; add new tests
- Modify: `server/orchestrator/mesh/mesh_test.go` — add `NodeRegistry` sub-tests for NodeID/Name/Zone and `NextFreeNodeID`

**Interfaces:**
- Produces: `NodeInfo.NodeID uint8`, `NodeInfo.Name string`, `NodeInfo.Zone string`
- Produces: `NodeRegistry.AssignNode(mac []byte, nodeId uint8, name, zone string)`
- Produces: `NodeRegistry.NextFreeNodeID() uint8` — returns lowest unused 1-255; returns 0 if all taken
- Produces: `MeshServer.ApproveEnrollment(macStr string, params ApprovalParams) error`
- Produces: `ApprovalParams{NodeID uint8, Name string, Zone string}` — NodeID 0 means auto-assign

**Context for implementer:**

**`NodeInfo` and `persistedNode` additions:**
```go
type NodeInfo struct {
    MAC         []byte    `json:"mac"`
    MACString   string    `json:"macString"`
    AdapterType int32     `json:"adapterType"`
    Uptime      uint32    `json:"uptime"`
    LastSeen    time.Time `json:"lastSeen"`
    HopCount    uint32    `json:"hopCount"`
    NodeID      uint8     `json:"nodeId,omitempty"`
    Name        string    `json:"name,omitempty"`
    Zone        string    `json:"zone,omitempty"`
}

type persistedNode struct {
    MAC         string    `json:"mac"`
    AdapterType int32     `json:"adapterType"`
    Uptime      uint32    `json:"uptime"`
    LastSeen    time.Time `json:"lastSeen"`
    HopCount    uint32    `json:"hopCount"`
    NodeID      uint8     `json:"nodeId,omitempty"`
    Name        string    `json:"name,omitempty"`
    Zone        string    `json:"zone,omitempty"`
}
```

**`UpdateNode` — sticky fields:** After this change, `UpdateNode` must NOT overwrite `NodeID`/`Name`/`Zone` for existing nodes:
```go
// existing fields update:
node.AdapterType = adapterType
node.Uptime = uptime
node.LastSeen = time.Now()
node.HopCount = hopCount
// NodeID, Name, Zone are sticky — only AssignNode may set them
```

**`AssignNode`:**
```go
func (nr *NodeRegistry) AssignNode(mac []byte, nodeId uint8, name, zone string) {
    nr.mu.Lock()
    defer nr.mu.Unlock()
    macStr := macToString(mac)
    node, exists := nr.nodes[macStr]
    if !exists {
        node = &NodeInfo{
            MAC:       make([]byte, len(mac)),
            MACString: macStr,
        }
        copy(node.MAC, mac)
        nr.nodes[macStr] = node
    }
    node.NodeID = nodeId
    node.Name = name
    node.Zone = zone
}
```

**`NextFreeNodeID`:**
```go
func (nr *NodeRegistry) NextFreeNodeID() uint8 {
    nr.mu.RLock()
    defer nr.mu.RUnlock()
    used := make(map[uint8]bool)
    for _, n := range nr.nodes {
        if n.NodeID > 0 {
            used[n.NodeID] = true
        }
    }
    for id := uint8(1); id <= 255; id++ {
        if !used[id] {
            return id
        }
    }
    return 0 // all 255 IDs in use
}
```

**`ApprovalParams` and updated `ApproveEnrollment`:**
```go
type ApprovalParams struct {
    NodeID uint8
    Name   string
    Zone   string
}

func (ms *MeshServer) ApproveEnrollment(macStr string, params ApprovalParams) error {
    node, err := ms.authRegistry.Approve(macStr)
    if err != nil {
        return err
    }

    // Auto-assign nodeId if not provided
    nodeId := params.NodeID
    if nodeId == 0 {
        nodeId = ms.nodeRegistry.NextFreeNodeID()
        if nodeId == 0 {
            slog.Warn("All node IDs in use; node will have ID 0", "mac", macStr)
        }
    }

    // Assign in node registry (creates entry if not yet seen)
    ms.nodeRegistry.AssignNode(node.MAC[:], nodeId, params.Name, params.Zone)

    if ms.serialComm != nil {
        // Send JOIN_ACK
        ackMsg := &MeshMessage{
            MessageType:      MessageTypeJoinAck,
            TargetMacAddress: node.MAC[:],
            PublicKey:        node.PublicKey[:],
        }
        if err := ms.serialComm.WriteFrame(ackMsg); err != nil {
            slog.Warn("Failed to send JOIN_ACK", "mac", macStr, "error", err)
        }

        // Send OP_NODE_ID_SET immediately after JOIN_ACK
        if nodeId > 0 {
            payload := make([]byte, MaxDataLength)
            payload[0] = OpNodeIdSet     // 0xC0
            copy(payload[1:7], node.MAC[:]) // target MAC
            payload[7] = nodeId
            idMsg := &MeshMessage{
                MessageType: MessageTypeSerialCmdBroadcast,
                DataType:    AdapterTypeSerial,
                Data:        payload,
            }
            if err := ms.serialComm.WriteFrame(idMsg); err != nil {
                slog.Warn("Failed to send OP_NODE_ID_SET", "mac", macStr, "nodeId", nodeId, "error", err)
            }
        }
    }

    slog.Info("Enrollment approved", "mac", macStr, "nodeId", nodeId, "name", params.Name)
    if ms.authPath != "" {
        return ms.authRegistry.Persist(ms.authPath)
    }
    return nil
}
```

**`approveEnrollment` API handler** — parse body, pass params:
```go
type ApprovalRequest struct {
    NodeID uint8  `json:"nodeId"`
    Name   string `json:"name"`
    Zone   string `json:"zone"`
}

func (api *APIServer) approveEnrollment(w http.ResponseWriter, r *http.Request) {
    mac := mux.Vars(r)["mac"]
    var req ApprovalRequest
    if r.Body != nil {
        _ = json.NewDecoder(r.Body).Decode(&req) // body is optional; ignore decode errors
    }
    params := mesh.ApprovalParams{NodeID: req.NodeID, Name: req.Name, Zone: req.Zone}
    if err := api.meshServer.ApproveEnrollment(mac, params); err != nil {
        api.writeError(w, http.StatusBadRequest, err.Error())
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{
        Success: true,
        Message: fmt.Sprintf("Enrollment approved for %s", mac),
    })
}
```

**Existing test updates:** `TestApproveEnrollment_SendsJoinAckWithPubKey`, `TestApproveEnrollment_UnknownMAC_ReturnsError`, and `TestApproveEnrollment_NilSerialComm_Succeeds` all call `ms.ApproveEnrollment(macStr)`. Update to `ms.ApproveEnrollment(macStr, ApprovalParams{})` (zero params = auto-assign).

`TestApproveEnrollment_SendsJoinAckWithPubKey` also calls `decodeWrittenFrame` — after the change, TWO frames are written (JOIN_ACK + OP_NODE_ID_SET). The test currently reads one frame. Update to read both:
```go
// First frame: JOIN_ACK
joinAck := decodeWrittenFrame(t, mockPort)
// ... existing assertions on joinAck ...

// Second frame: OP_NODE_ID_SET
nodeIdMsg := decodeWrittenFrame(t, mockPort)
if nodeIdMsg.Data[0] != byte(OpNodeIdSet) {
    t.Errorf("second frame opcode = 0x%02x, want 0x%02x", nodeIdMsg.Data[0], OpNodeIdSet)
}
```

Also update `Load` to populate `NodeID`/`Name`/`Zone` from persisted entries — add to the `Load` loop:
```go
nr.nodes[e.MAC] = &NodeInfo{
    MAC:         mac,
    MACString:   e.MAC,
    AdapterType: e.AdapterType,
    Uptime:      e.Uptime,
    LastSeen:    e.LastSeen,
    HopCount:    e.HopCount,
    NodeID:      e.NodeID,
    Name:        e.Name,
    Zone:        e.Zone,
}
```

- [ ] **Step 1: Write failing tests**

  In `server/orchestrator/mesh/mesh_test.go`, add inside `TestNodeRegistry`:
  ```go
  t.Run("AssignNode_SetsIdentityFields", func(t *testing.T) {
      registry := NewNodeRegistry()
      mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
      registry.AssignNode(mac, 7, "entrance-left", "lobby")
      node, ok := registry.GetNode(mac)
      if !ok { t.Fatal("node not found after AssignNode") }
      if node.NodeID != 7 { t.Errorf("NodeID: got %d, want 7", node.NodeID) }
      if node.Name != "entrance-left" { t.Errorf("Name: got %q", node.Name) }
      if node.Zone != "lobby" { t.Errorf("Zone: got %q", node.Zone) }
  })

  t.Run("UpdateNode_DoesNotOverwriteAssignedFields", func(t *testing.T) {
      registry := NewNodeRegistry()
      mac := []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
      registry.AssignNode(mac, 3, "stage-left", "main")
      registry.UpdateNode(mac, AdapterTypePIR, 1000, 2)
      node, ok := registry.GetNode(mac)
      if !ok { t.Fatal("node not found") }
      if node.NodeID != 3 { t.Errorf("NodeID overwritten: got %d, want 3", node.NodeID) }
      if node.Name != "stage-left" { t.Errorf("Name overwritten: got %q", node.Name) }
  })

  t.Run("NextFreeNodeID_ReturnsOne_WhenEmpty", func(t *testing.T) {
      registry := NewNodeRegistry()
      if id := registry.NextFreeNodeID(); id != 1 {
          t.Errorf("NextFreeNodeID: got %d, want 1", id)
      }
  })

  t.Run("NextFreeNodeID_SkipsUsedIds", func(t *testing.T) {
      registry := NewNodeRegistry()
      registry.AssignNode([]byte{0x01, 0, 0, 0, 0, 0}, 1, "", "")
      registry.AssignNode([]byte{0x02, 0, 0, 0, 0, 0}, 2, "", "")
      if id := registry.NextFreeNodeID(); id != 3 {
          t.Errorf("NextFreeNodeID: got %d, want 3", id)
      }
  })
  ```

  In `server/orchestrator/mesh/server_enrollment_test.go`, add:
  ```go
  func TestApproveEnrollment_SendsNodeIdSet_AfterJoinAck(t *testing.T) {
      ms := newTestMeshServer(t)
      mockPort := NewMockSerialPort()
      ms.serialComm = NewSerialComm(mockPort)

      macStr, _ := enrollTestNode(t, ms)

      params := ApprovalParams{NodeID: 7, Name: "test-node", Zone: "lobby"}
      if err := ms.ApproveEnrollment(macStr, params); err != nil {
          t.Fatalf("ApproveEnrollment returned error: %v", err)
      }

      // First frame: JOIN_ACK
      _ = decodeWrittenFrame(t, mockPort)

      // Second frame: OP_NODE_ID_SET
      idMsg := decodeWrittenFrame(t, mockPort)
      if idMsg.Data[0] != byte(OpNodeIdSet) {
          t.Errorf("second frame opcode = 0x%02x, want 0x%02x", idMsg.Data[0], OpNodeIdSet)
      }
      if idMsg.Data[7] != 7 {
          t.Errorf("nodeId in frame = %d, want 7", idMsg.Data[7])
      }
  }

  func TestApproveEnrollment_AutoAssignsNodeId_WhenZero(t *testing.T) {
      ms := newTestMeshServer(t)
      macStr, _ := enrollTestNode(t, ms)

      params := ApprovalParams{} // NodeID = 0 → auto-assign
      if err := ms.ApproveEnrollment(macStr, params); err != nil {
          t.Fatalf("ApproveEnrollment returned error: %v", err)
      }

      wantMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF} // from enrollTestNode
      node, ok := ms.GetNodeRegistry().GetNode(wantMAC)
      if !ok { t.Fatal("node not registered") }
      if node.NodeID == 0 {
          t.Error("NodeID should be auto-assigned (>0)")
      }
  }
  ```

- [ ] **Step 2: Run tests to verify they fail**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestNodeRegistry|TestApproveEnrollment" -v 2>&1 | tail -20
  ```

  Expected: compile errors (AssignNode/NextFreeNodeID not defined; ApproveEnrollment wrong arity).

- [ ] **Step 3: Add `OpNodeIdSet` to `constants.go`**

  After `OpNodeHealth`:
  ```go
  OpNodeIdSet    byte = 0xC0 // Server → node: assign logical ID; data: [C0][6B targetMAC][1B nodeId]
  ```

- [ ] **Step 4: Update `NodeInfo`, `persistedNode`, add `AssignNode`/`NextFreeNodeID`, fix `UpdateNode` and `Load` in `node_registry.go`**

  Apply all changes described in Context above.

- [ ] **Step 5: Add `ApprovalParams` and update `ApproveEnrollment` in `server.go`**

  Apply changes described in Context above.

- [ ] **Step 6: Update `approveEnrollment` handler in `api.go`**

  Add `ApprovalRequest` struct and body parsing as shown in Context.

- [ ] **Step 7: Update existing enrollment tests for new signature**

  Update all `ms.ApproveEnrollment(macStr)` calls to `ms.ApproveEnrollment(macStr, ApprovalParams{})`.
  Update `TestApproveEnrollment_SendsJoinAckWithPubKey` to read two frames.

- [ ] **Step 8: Run targeted tests to verify they pass**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... \
    -run "TestNodeRegistry|TestApproveEnrollment" -v 2>&1 | tail -20
  ```

  Expected: all targeted tests PASS.

- [ ] **Step 9: Run full server test suite**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 2>&1 | tail -5
  ```

  Expected: `ok github.com/superbrobenji/motionServer/mesh` with no failures.

- [ ] **Step 10: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/constants.go \
    server/orchestrator/mesh/node_registry.go \
    server/orchestrator/mesh/server.go \
    server/orchestrator/mesh/api.go \
    server/orchestrator/mesh/server_enrollment_test.go \
    server/orchestrator/mesh/mesh_test.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit \
    -m "feat(server): node identity — NodeID/Name/Zone, ApproveEnrollment params, OP_NODE_ID_SET"
  ```
