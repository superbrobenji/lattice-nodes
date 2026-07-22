# Phase 9: Dual Master (Backup Master) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow two ESP32 master nodes to run simultaneously — sensor nodes route to whichever has a lower hop count, and the server transparently fails over outgoing commands to the secondary serial port if the primary is silent for 75 seconds.

**Architecture:** Firmware: add `DUAL_MASTER_MODE` runtime flag to `Mesh`; modify `processMasterBeacon` to learn and accept a secondary master MAC via TOFU, suppressing the "multiple masters" warning. EEPROM layout bumps to v3 to persist the secondary MAC. Server: `MeshServer` grows a second `SerialComm`; both read goroutines process into the same registries; `activeOutboundComm()` selects the port that received a frame most recently, falling back to secondary after 75 s of primary silence.

**Tech Stack:** Go 1.22, ESP32 C++ (Arduino-ESP32), nanopb/protobuf3, GTest, mbedTLS, ESP-NOW.

## Global Constraints

- Go module root: `motionSensorServer/server/orchestrator/` — all Go paths relative to this
- Go test command: `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1`
- Firmware test command: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure` (run from `Planetopia-nodes/`)
- `AdapterTypeUnknown int32 = -1`; `AdapterTypePIR int32 = 0`
- EEPROM total size: 512 bytes; v2 layout ends at address 496 (NODE_ID); v3 adds `KNOWN_MASTER_MAC_SECONDARY` at 497–502; `CURRENT_SCHEMA_VERSION` bumps from 2 to 3
- Primary failover threshold: exactly 75 seconds (`75 * time.Second`)
- No `gh` commands. Git identity for every commit: `git config user.email "49689582+superbrobenji@users.noreply.github.com"` + `git config user.name "superbrobenji"` (set per-repo before first commit)
- clang-format: single-statement `if` bodies on separate lines

---

## File Map

| File | Repo | Change |
|---|---|---|
| `mesh/server.go` | motionSensorServer | Add `secondaryPort`, `secondarySerialComm`, frame-time tracking; refactor `messageProcessor`; add `activeOutboundComm()`; route all outgoing writes through it; `SerialStatus()` |
| `mesh/api_v1_status.go` | motionSensorServer | Add `serial.primary`/`serial.secondary` block |
| `main.go` | motionSensorServer | Read `SERIAL_PORT_SECONDARY` + `DUAL_MASTER_ENABLED` env vars; pass to config |
| `mesh/server_test.go` | motionSensorServer | Add `activeOutboundComm` failover tests |
| `mesh/api_v1_status_test.go` (new) | motionSensorServer | Status endpoint tests |
| `main/src/persistence/EEPROM_Manager.h` | Planetopia-nodes | Add `KNOWN_MASTER_MAC_SECONDARY = 497`, bump `CURRENT_SCHEMA_VERSION = 3`, declare 3 new methods |
| `main/src/persistence/EEPROM_Manager.cpp` | Planetopia-nodes | Implement 3 new methods + v2→v3 migration |
| `tests/unit/test_eeprom_manager.cpp` | Planetopia-nodes | 3 tests for secondary MAC |
| `main/project_config.h` | Planetopia-nodes | Add `DUAL_MASTER_MODE = false` |
| `main/src/Mesh/Mesh.h` | Planetopia-nodes | Add `knownMasterMacSecondary`, `hasMasterMacSecondary`, `_dualMasterMode`, `setDualMasterMode()` |
| `main/src/Mesh/Mesh.cpp` | Planetopia-nodes | Constructor + `loadPersistentState()` secondary MAC |
| `tests/mocks/firmware_stubs.cpp` | Planetopia-nodes | Add new fields to test constructor |
| `tests/mocks/mesh_logic_impl.cpp` | Planetopia-nodes | Modify `processMasterBeacon` for dual mode |
| `tests/unit/test_mesh_logic.cpp` | Planetopia-nodes | 5 dual-master beacon tests |

---

### Task 1: Secondary serial port in MeshServer

**Repo:** `motionSensorServer/server/orchestrator/`

**Files:**
- Modify: `mesh/server.go`
- Modify: `mesh/server_test.go`

**Interfaces:**
- Produces: `func (ms *MeshServer) SerialStatus() (primary bool, secondary bool)` — returns whether primary/secondary comms are active
- Produces: `func (ms *MeshServer) activeOutboundComm() *SerialComm` — returns the comm to use for outgoing frames
- Produces: `MeshServerConfig.SerialPortSecondary string` — consumed by Task 2

- [ ] **Step 1: Write three failing tests in `mesh/server_test.go`**

Add after existing tests:

```go
func TestActiveOutboundComm_ReturnsPrimary_WhenSecondaryNotConfigured(t *testing.T) {
	ms := newTestMeshServer(t)
	mock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(mock)

	comm := ms.activeOutboundComm()
	if comm != ms.serialComm {
		t.Error("activeOutboundComm() must return primary when no secondary configured")
	}
}

func TestActiveOutboundComm_ReturnsPrimary_WhenPrimaryIsRecent(t *testing.T) {
	ms := newTestMeshServer(t)
	primaryMock := NewMockSerialPort()
	secondaryMock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(primaryMock)
	ms.secondarySerialComm = NewSerialComm(secondaryMock)
	// Primary received a frame 10 seconds ago — well within the 75s threshold
	ms.primaryLastFrameAt = time.Now().Add(-10 * time.Second)

	comm := ms.activeOutboundComm()
	if comm != ms.serialComm {
		t.Error("activeOutboundComm() must return primary when primary is recent")
	}
}

func TestActiveOutboundComm_FailsOverToSecondary_AfterPrimaryTimeout(t *testing.T) {
	ms := newTestMeshServer(t)
	primaryMock := NewMockSerialPort()
	secondaryMock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(primaryMock)
	ms.secondarySerialComm = NewSerialComm(secondaryMock)
	// Primary last heard 76 seconds ago — over the 75s threshold
	ms.primaryLastFrameAt = time.Now().Add(-76 * time.Second)

	comm := ms.activeOutboundComm()
	if comm != ms.secondarySerialComm {
		t.Error("activeOutboundComm() must return secondary after primary timeout")
	}
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestActiveOutboundComm
```

Expected: compile error — `secondarySerialComm`, `primaryLastFrameAt`, `activeOutboundComm` not defined.

- [ ] **Step 3: Add new fields to `MeshServer` struct in `mesh/server.go`**

In the `MeshServer` struct, after `serialComm *SerialComm`, add:

```go
secondaryPort        string
secondarySerialComm  *SerialComm
secondaryConnected   bool

frameTimeMu          sync.Mutex // protects primaryLastFrameAt / secondaryLastFrameAt
primaryLastFrameAt   time.Time
secondaryLastFrameAt time.Time
```

- [ ] **Step 4: Add `SerialPortSecondary` to `MeshServerConfig` in `mesh/server.go`**

In `MeshServerConfig`, after `SerialPort string`, add:

```go
SerialPortSecondary string // empty = single-master mode
```

- [ ] **Step 5: Populate `secondaryPort` in `NewMeshServer` in `mesh/server.go`**

In the `return &MeshServer{...}` block, after `serialPort: config.SerialPort,`, add:

```go
secondaryPort: config.SerialPortSecondary,
```

- [ ] **Step 6: Add `activeOutboundComm()` method to `mesh/server.go`**

Add this method after `Stop()`:

```go
// activeOutboundComm returns the SerialComm to use for outgoing frames.
// When a secondary port is configured, switches to secondary if primary has
// been silent for more than 75 seconds.
func (ms *MeshServer) activeOutboundComm() *SerialComm {
	if ms.secondarySerialComm == nil {
		return ms.serialComm
	}
	ms.frameTimeMu.Lock()
	primaryAge := time.Since(ms.primaryLastFrameAt)
	ms.frameTimeMu.Unlock()
	const failoverThreshold = 75 * time.Second
	if primaryAge > failoverThreshold {
		return ms.secondarySerialComm
	}
	return ms.serialComm
}
```

- [ ] **Step 7: Run the three new tests to verify they pass**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestActiveOutboundComm
```

Expected: all 3 PASS.

- [ ] **Step 8: Refactor `messageProcessor` to accept a comm + label in `mesh/server.go`**

Change the signature from:

```go
func (ms *MeshServer) messageProcessor() {
```

to:

```go
func (ms *MeshServer) messageProcessor(comm *SerialComm, label string) {
```

Replace all `ms.serialComm` inside `messageProcessor` with `comm`. After the line:

```go
if consecutiveErrors > 0 {
```

(when `consecutiveErrors > maxConsecutiveErrors` resets after recovery), find the line that currently reads `SetSerialConnected(false)` (inside the error branch) and update it:

```go
if label == "primary" {
    SetSerialConnected(false)
}
```

After the successful-read path (immediately after the `consecutiveErrors` reset block), record the frame time:

```go
ms.frameTimeMu.Lock()
if label == "primary" {
    ms.primaryLastFrameAt = time.Now()
} else {
    ms.secondaryLastFrameAt = time.Now()
}
ms.frameTimeMu.Unlock()
```

- [ ] **Step 9: Update `Start()` to pass the comm and label, and open secondary if configured**

In `Start()`, change:

```go
go ms.messageProcessor()
```

to:

```go
go ms.messageProcessor(ms.serialComm, "primary")
```

After that line, add secondary port setup:

```go
if ms.secondaryPort != "" {
    secondaryPhysPort, secErr := serial.Open(ms.secondaryPort, mode)
    if secErr != nil {
        slog.Warn("Failed to open secondary serial port — continuing single-master",
            "port", ms.secondaryPort, "error", secErr)
    } else {
        ms.secondarySerialComm = NewSerialComm(secondaryPhysPort)
        ms.secondaryConnected = true
        ms.wg.Add(1)
        go ms.messageProcessor(ms.secondarySerialComm, "secondary")
        slog.Info("Secondary serial port opened", "port", ms.secondaryPort)
    }
}
```

- [ ] **Step 10: Update `Stop()` to close secondary**

In `Stop()`, after `ms.serialComm.Close()`:

```go
if ms.secondarySerialComm != nil {
    ms.secondarySerialComm.Close()
    ms.secondaryConnected = false
}
```

- [ ] **Step 11: Route all outgoing `WriteFrame` calls through `activeOutboundComm()`**

In `mesh/server.go`, replace every `ms.serialComm.WriteFrame(` with `ms.activeOutboundComm().WriteFrame(`.

There are occurrences at (approximately) these lines — find them all:
- `ApproveEnrollment`: three `WriteFrame` calls (JOIN_ACK, OP_NODE_ID_SET, OP_CONFIG_SET)
- `RejectEnrollment`: one `WriteFrame`
- `SendMessage`: one `WriteFrame`
- `broadcastNodeId` (or similar internal method): one `WriteFrame`
- `RequestHealthReports`: one `WriteFrame`

Keep all `if ms.serialComm != nil` guard checks as-is — they guard against the server not being started, not against failover. Only the `WriteFrame` call itself changes.

- [ ] **Step 12: Add `SerialStatus()` method to `mesh/server.go`**

```go
// SerialStatus returns the connection state of primary and secondary serial ports.
func (ms *MeshServer) SerialStatus() (primary bool, secondary bool) {
	ms.mu.RLock()
	defer ms.mu.RUnlock()
	return ms.serialComm != nil, ms.secondaryConnected
}
```

- [ ] **Step 13: Run full suite to verify no regressions**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1
```

Expected: all tests PASS.

- [ ] **Step 14: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer config user.email "49689582+superbrobenji@users.noreply.github.com"
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer config user.name "superbrobenji"
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer
git add server/orchestrator/mesh/server.go server/orchestrator/mesh/server_test.go
git commit -m "feat: add secondary serial port with 75s primary-failover to MeshServer"
```

---

### Task 2: Status endpoint dual master + main.go env vars

**Repo:** `motionSensorServer/server/orchestrator/`

**Files:**
- Modify: `mesh/api_v1_status.go`
- Create: `mesh/api_v1_status_test.go`
- Modify: `main.go`

**Interfaces:**
- Consumes (Task 1): `func (ms *MeshServer) SerialStatus() (primary bool, secondary bool)`
- Consumes (Task 1): `MeshServerConfig.SerialPortSecondary string`

- [ ] **Step 1: Create `mesh/api_v1_status_test.go` with three failing tests**

```go
package mesh

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestV1Status_SingleMaster_SerialBlockShowsPrimaryOnly(t *testing.T) {
	api, ms := newV1TestServer(t)
	mock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(mock)
	// secondarySerialComm remains nil — single master mode

	w := v1Request(t, api, "GET", "/api/v1/status", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("status returned %d", w.Code)
	}

	var resp APIResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data, _ := json.Marshal(resp.Data)
	var status struct {
		Serial struct {
			Primary   string `json:"primary"`
			Secondary string `json:"secondary"`
		} `json:"serial"`
	}
	if err := json.Unmarshal(data, &status); err != nil {
		t.Fatalf("unmarshal status: %v", err)
	}
	if status.Serial.Primary != "connected" {
		t.Errorf("serial.primary = %q, want %q", status.Serial.Primary, "connected")
	}
	if status.Serial.Secondary != "not_configured" {
		t.Errorf("serial.secondary = %q, want %q", status.Serial.Secondary, "not_configured")
	}
}

func TestV1Status_DualMaster_SecondaryConnected(t *testing.T) {
	api, ms := newV1TestServer(t)
	primaryMock := NewMockSerialPort()
	secondaryMock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(primaryMock)
	ms.secondarySerialComm = NewSerialComm(secondaryMock)
	ms.secondaryConnected = true

	w := v1Request(t, api, "GET", "/api/v1/status", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("status returned %d", w.Code)
	}

	var resp APIResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data, _ := json.Marshal(resp.Data)
	var status struct {
		Serial struct {
			Primary   string `json:"primary"`
			Secondary string `json:"secondary"`
		} `json:"serial"`
	}
	if err := json.Unmarshal(data, &status); err != nil {
		t.Fatalf("unmarshal status: %v", err)
	}
	if status.Serial.Primary != "connected" {
		t.Errorf("serial.primary = %q, want %q", status.Serial.Primary, "connected")
	}
	if status.Serial.Secondary != "connected" {
		t.Errorf("serial.secondary = %q, want %q", status.Serial.Secondary, "connected")
	}
}

func TestV1Status_DualMaster_SecondaryDisconnected(t *testing.T) {
	api, ms := newV1TestServer(t)
	primaryMock := NewMockSerialPort()
	ms.serialComm = NewSerialComm(primaryMock)
	// secondarySerialComm is nil but secondaryPort is set — secondary configured but failed to open
	ms.secondaryPort = "/dev/ttyUSB1"
	ms.secondaryConnected = false

	w := v1Request(t, api, "GET", "/api/v1/status", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("status returned %d", w.Code)
	}

	var resp APIResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data, _ := json.Marshal(resp.Data)
	var status struct {
		Serial struct {
			Primary   string `json:"primary"`
			Secondary string `json:"secondary"`
		} `json:"serial"`
	}
	if err := json.Unmarshal(data, &status); err != nil {
		t.Fatalf("unmarshal status: %v", err)
	}
	if status.Serial.Secondary != "disconnected" {
		t.Errorf("serial.secondary = %q, want %q", status.Serial.Secondary, "disconnected")
	}
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestV1Status
```

Expected: FAIL — response has no `serial` key yet.

- [ ] **Step 3: Update `v1Status` in `mesh/api_v1_status.go`**

Replace the full function body with:

```go
func (api *APIServer) v1Status(w http.ResponseWriter, r *http.Request) {
	timeout := api.meshServer.GetHealthTimeout()
	allNodes := api.meshServer.GetNodeRegistry().GetAllNodes()
	total := 0
	online := 0
	for _, n := range allNodes {
		if n.NodeID > 0 && n.Status != "replaced" {
			total++
			if isOnline(n, timeout) {
				online++
			}
		}
	}

	primaryConnected, secondaryConnected := api.meshServer.SerialStatus()

	primaryStatus := "disconnected"
	if primaryConnected {
		primaryStatus = "connected"
	}

	secondaryStatus := "not_configured"
	if api.meshServer.secondaryPort != "" {
		if secondaryConnected {
			secondaryStatus = "connected"
		} else {
			secondaryStatus = "disconnected"
		}
	}

	api.writeJSON(w, http.StatusOK, APIResponse{
		Success: true,
		Data: map[string]interface{}{
			"serial": map[string]string{
				"primary":   primaryStatus,
				"secondary": secondaryStatus,
			},
			"nodes": map[string]int{
				"total":   total,
				"online":  online,
				"offline": total - online,
			},
			"mesh": map[string]bool{
				"masterOnline": api.meshServer.IsRunning(),
			},
		},
	})
}
```

Note: `api.meshServer.secondaryPort` is an unexported field — since `APIServer` is in the same package (`mesh`), this is fine.

- [ ] **Step 4: Run the three status tests to verify they pass**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestV1Status
```

Expected: all 3 PASS.

- [ ] **Step 5: Update `main.go` to read secondary port env vars**

In `main.go`, after the `serialPort` flag declaration (around line 39), add:

```go
serialPortSecondary := envOrDefault("SERIAL_PORT_SECONDARY", "")
dualMasterEnabled := os.Getenv("DUAL_MASTER_ENABLED") == "true"
```

Update `meshConfig` construction to include the secondary port:

```go
meshConfig := mesh.MeshServerConfig{
    SerialPort:          *serialPort,
    SerialPortSecondary: func() string {
        if dualMasterEnabled && serialPortSecondary != "" {
            return serialPortSecondary
        }
        return ""
    }(),
    BaudRate:         *baudRate,
    HealthTimeout:    30 * time.Second,
    EventStore:       eventStore,
    AuthRegistryPath: *authRegistry,
    NodeRegistryPath: *nodeRegistry,
}
```

Also add a log line after the existing `slog.Info("Serial", ...)`:

```go
if dualMasterEnabled && serialPortSecondary != "" {
    slog.Info("Secondary serial port", "port", serialPortSecondary)
}
```

- [ ] **Step 6: Run full suite to verify no regressions**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1
```

Expected: all tests PASS.

- [ ] **Step 7: Commit**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer
git add server/orchestrator/mesh/api_v1_status.go server/orchestrator/mesh/api_v1_status_test.go server/orchestrator/main.go
git commit -m "feat: dual master status in GET /api/v1/status; SERIAL_PORT_SECONDARY + DUAL_MASTER_ENABLED env vars"
```

---

### Task 3: EEPROM layout v3 — secondary master MAC

**Repo:** `Planetopia-nodes/`

**Files:**
- Modify: `main/src/persistence/EEPROM_Manager.h`
- Modify: `main/src/persistence/EEPROM_Manager.cpp`
- Modify: `tests/unit/test_eeprom_manager.cpp`

**Interfaces:**
- Produces: `bool EEPROM_Manager::loadKnownMasterMacSecondary(uint8_t mac[6])` — returns false if unset (0xFF×6)
- Produces: `void EEPROM_Manager::saveKnownMasterMacSecondary(const uint8_t mac[6])`
- Produces: `void EEPROM_Manager::clearKnownMasterMacSecondary()`
- Produces: `EEPROM_ADDRESSES::KNOWN_MASTER_MAC_SECONDARY = 497`

- [ ] **Step 1: Write three failing tests in `tests/unit/test_eeprom_manager.cpp`**

Add after the existing `KnownMasterMac` tests (search for `clearKnownMasterMac` to find the end of that group):

```cpp
TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_UnsetReturnsAlFalse) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  uint8_t mac[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(mac);
  EXPECT_FALSE(found) << "Blank EEPROM must report secondary master MAC as unset";
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_SaveAndLoad_RoundTrip) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr.saveKnownMasterMacSecondary(expected);

  uint8_t loaded[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_Clear_ResetsToUnset) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr.saveKnownMasterMacSecondary(mac);
  mgr.clearKnownMasterMacSecondary();

  uint8_t loaded[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(loaded);
  EXPECT_FALSE(found) << "After clear, secondary master MAC must report unset";
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: compile error — `loadKnownMasterMacSecondary` not declared.

- [ ] **Step 3: Update `EEPROM_Manager.h`**

In the EEPROM layout comment block (around line 12), add after the existing `KNOWN_MASTER_MAC` comment line:

```cpp
// 497   KNOWN_MASTER_MAC_SECONDARY (6 bytes, ends 502) — TOFU secondary master MAC (0xFF×6 = unset)
// Total used: 503 bytes — fits in 512
```

In `namespace EEPROM_ADDRESSES` (around line 32), add after `KNOWN_MASTER_MAC`:

```cpp
constexpr uint16_t KNOWN_MASTER_MAC_SECONDARY = 497; // 6 bytes: TOFU secondary master MAC (0xFF×6 = unset, ends 502)
```

In `namespace EEPROM_SIZES`, change:

```cpp
constexpr uint8_t CURRENT_SCHEMA_VERSION = 2; // Current EEPROM schema version
```

to:

```cpp
constexpr uint8_t CURRENT_SCHEMA_VERSION = 3; // Current EEPROM schema version
```

In the `class EEPROM_Manager` public section, after `clearKnownMasterMac();`, add:

```cpp
bool loadKnownMasterMacSecondary(uint8_t mac[6]);
void saveKnownMasterMacSecondary(const uint8_t mac[6]);
void clearKnownMasterMacSecondary();
```

- [ ] **Step 4: Update `EEPROM_Manager.cpp` — add v2→v3 migration**

In the migration block in `init()`, find the comment:

```cpp
// Future: add migration handlers for v2→v3, v3→v4, etc. here
```

Replace it with:

```cpp
if (storedVersion <= 2) {
    // v2→v3 migration: zero-fill the new secondary master MAC slot
    Logger::logln("EEPROM", "v2→v3 migration: initialising secondary master MAC slot", LogLevel::LOG_INFO);
    for (uint16_t i = 0; i < 6; ++i) {
        EEPROM.write(EEPROM_ADDRESSES::KNOWN_MASTER_MAC_SECONDARY + i, 0xFF);
    }
}
```

Also update the blank-EEPROM log message from `"Fresh EEPROM — version 2 written"` to `"Fresh EEPROM — version 3 written"`.

- [ ] **Step 5: Add the three new methods to `EEPROM_Manager.cpp`**

Add after `clearKnownMasterMac()`:

```cpp
bool EEPROM_Manager::loadKnownMasterMacSecondary(uint8_t mac[6]) {
  if (!ensureInitialized())
    return false;
  for (int i = 0; i < 6; ++i)
    mac[i] = EEPROM.read(EEPROM_ADDRESSES::KNOWN_MASTER_MAC_SECONDARY + i);
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void EEPROM_Manager::saveKnownMasterMacSecondary(const uint8_t mac[6]) {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 6; ++i)
    EEPROM.write(EEPROM_ADDRESSES::KNOWN_MASTER_MAC_SECONDARY + i, mac[i]);
  EEPROM.commit();
  logOperation("Known secondary master MAC saved");
}

void EEPROM_Manager::clearKnownMasterMacSecondary() {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 6; ++i)
    EEPROM.write(EEPROM_ADDRESSES::KNOWN_MASTER_MAC_SECONDARY + i, 0xFF);
  markDirty();
}
```

- [ ] **Step 6: Run tests to verify all three pass**

```
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all 3 new tests PASS, no existing tests broken.

- [ ] **Step 7: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes config user.email "49689582+superbrobenji@users.noreply.github.com"
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes config user.name "superbrobenji"
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
git add main/src/persistence/EEPROM_Manager.h main/src/persistence/EEPROM_Manager.cpp tests/unit/test_eeprom_manager.cpp
git commit -m "feat: EEPROM v3 — secondary master MAC at address 497 with v2→v3 migration"
```

---

### Task 4: DUAL_MASTER_MODE flag + dual beacon acceptance

**Repo:** `Planetopia-nodes/`

**Files:**
- Modify: `main/project_config.h`
- Modify: `main/src/Mesh/Mesh.h`
- Modify: `main/src/Mesh/Mesh.cpp`
- Modify: `tests/mocks/firmware_stubs.cpp`
- Modify: `tests/mocks/mesh_logic_impl.cpp`
- Modify: `tests/unit/test_mesh_logic.cpp`

**Interfaces:**
- Consumes (Task 3): `EEPROM_Manager::loadKnownMasterMacSecondary`, `saveKnownMasterMacSecondary`
- Produces: `mesh.setDualMasterMode(bool)` — sets `_dualMasterMode`; used in tests to enable dual mode without recompiling

- [ ] **Step 1: Write five failing tests in `tests/unit/test_mesh_logic.cpp`**

Add after the existing `BeaconRelay_NewerSeq_AllowsRelay` test:

```cpp
// --- Dual master mode ---

TEST_F(MeshLogicTest, DualMaster_SecondBeaconFromNewMAC_LearnedAsSecondary) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  // Learn primary via first beacon
  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));
  ASSERT_TRUE(mesh.hasMasterMac);

  // Second beacon from different MAC — must be learned as secondary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1));

  EXPECT_TRUE(mesh.hasMasterMacSecondary);
  EXPECT_EQ(memcmp(mesh.knownMasterMacSecondary, secondaryMac, 6), 0);
  // Primary must still be unchanged
  EXPECT_EQ(memcmp(mesh.knownMasterMac, primaryMac, 6), 0);
}

TEST_F(MeshLogicTest, DualMaster_BeaconFromPrimaryMAC_Accepted) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Beacon from primary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.processMasterBeacon(makeBeacon(primaryMac, 2, 1));

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known primary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_BeaconFromSecondaryMAC_Accepted) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Beacon from secondary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 2, 1));

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known secondary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_ImpostorMAC_Rejected_WhenBothMastersKnown) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t impostorMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Third distinct MAC while both masters fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.processMasterBeacon(makeBeacon(impostorMac, 1, 2));

  // Neither primary nor secondary should have changed
  EXPECT_EQ(memcmp(mesh.knownMasterMac, primaryMac, 6), 0);
  EXPECT_EQ(memcmp(mesh.knownMasterMacSecondary, secondaryMac, 6), 0);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore) << "Impostor beacon must not trigger relay";
}

TEST_F(MeshLogicTest, SingleMaster_SecondBeaconFromNewMAC_Rejected_WhenMasterAlive) {
  Mesh mesh;
  // _dualMasterMode defaults to false — no need to set
  const uint8_t knownMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t unknownMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.processMasterBeacon(makeBeacon(knownMac, 1, 1));

  // Second distinct MAC while single master still fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.processMasterBeacon(makeBeacon(unknownMac, 1, 2));

  EXPECT_EQ(memcmp(mesh.knownMasterMac, knownMac, 6), 0) << "Known master MAC must not change";
  EXPECT_FALSE(mesh.hasMasterMacSecondary);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: compile errors — `setDualMasterMode`, `hasMasterMacSecondary`, `knownMasterMacSecondary` not defined.

- [ ] **Step 3: Add `DUAL_MASTER_MODE` to `main/project_config.h`**

In `project_config.h`, after `STALE_MASTER_THRESHOLD_MS`, add:

```cpp
// Enable for deployments with two physically separate master nodes.
// When false (production default), standard single-master TOFU enforcement applies.
constexpr bool DUAL_MASTER_MODE = false;
```

- [ ] **Step 4: Add new fields and methods to `main/src/Mesh/Mesh.h`**

In the private section, after `bool hasMasterMac;`, add:

```cpp
uint8_t knownMasterMacSecondary[6];
bool hasMasterMacSecondary;
bool _dualMasterMode;
```

In the public section, after `bool getIsMaster() const { return isMaster; }`, add:

```cpp
void setDualMasterMode(bool value) { _dualMasterMode = value; }
bool getDualMasterMode() const { return _dualMasterMode; }
```

- [ ] **Step 5: Update `Mesh.cpp` constructor initializer list**

Find the constructor initializer list in `main/src/Mesh/Mesh.cpp` (around line 175). After `hasMasterMac(false),` add:

```cpp
knownMasterMacSecondary{}, hasMasterMacSecondary(false),
_dualMasterMode(planetopia::config::DUAL_MASTER_MODE),
```

In the constructor body, after `memset(knownMasterMac, 0xFF, 6);`, add:

```cpp
memset(knownMasterMacSecondary, 0xFF, 6);
```

- [ ] **Step 6: Update `loadPersistentState()` in `Mesh.cpp`**

In `loadPersistentState()`, after the `hasMasterMac` block, add:

```cpp
if (_dualMasterMode) {
    hasMasterMacSecondary =
        EEPROM_Manager::getInstance().loadKnownMasterMacSecondary(knownMasterMacSecondary);
    if (hasMasterMacSecondary) {
        Logger::logln("MESH", "Known secondary master MAC loaded from EEPROM", LogLevel::LOG_INFO);
    }
}
```

- [ ] **Step 7: Update the test constructor in `tests/mocks/firmware_stubs.cpp`**

In the `Mesh::Mesh()` initializer list (line 59–65), after `hasMasterMac(false),` add:

```cpp
knownMasterMacSecondary{}, hasMasterMacSecondary(false), _dualMasterMode(false),
```

- [ ] **Step 8: Modify `processMasterBeacon` in `tests/mocks/mesh_logic_impl.cpp`**

Replace the TOFU block (from the hop-count guard to the `lastSeenMasterMac` warning block) with:

```cpp
void Mesh::processMasterBeacon(const mesh_message& msg) {
  // Guard: drop beacon if hop count would overflow uint8_t or exceed limit
  if (msg.hopCount >= planetopia::config::MAX_HOPS) {
    Logger::logln("MESH", "Beacon hop count exceeded MAX_HOPS, dropping relay", LogLevel::LOG_WARN);
    return;
  }

  // --- TOFU master MAC enforcement ---
  bool fromPrimary = hasMasterMac && memcmp(msg.originMacAddress, knownMasterMac, 6) == 0;
  bool fromSecondary = _dualMasterMode && hasMasterMacSecondary &&
                       memcmp(msg.originMacAddress, knownMasterMacSecondary, 6) == 0;

  if (!hasMasterMac) {
    // First beacon ever — TOFU (fallback if JOIN_ACK path not taken, e.g. master node itself)
    memcpy(knownMasterMac, msg.originMacAddress, 6);
    hasMasterMac = true;
    EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    Logger::logln("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  } else if (!fromPrimary && !fromSecondary) {
    // Beacon from unrecognised MAC
    if (_dualMasterMode && !hasMasterMacSecondary) {
      // Second master TOFU — learn and save as secondary
      memcpy(knownMasterMacSecondary, msg.originMacAddress, 6);
      hasMasterMacSecondary = true;
      EEPROM_Manager::getInstance().saveKnownMasterMacSecondary(knownMasterMacSecondary);
      Logger::logln("MESH", "Secondary master MAC learned (TOFU)", LogLevel::LOG_INFO);
      // fall through to process this beacon as valid
    } else if (millis() - lastMasterBeaconReceivedMs < STALE_MASTER_THRESHOLD_MS) {
      // Known master(s) still fresh — reject unknown MAC
      Logger::logln("MESH", "Beacon from unexpected MAC rejected (master still alive)",
                    LogLevel::LOG_WARN);
      return;
    } else {
      // All known masters stale — accept as new primary (hotswap)
      Logger::logln("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
      memcpy(knownMasterMac, msg.originMacAddress, 6);
      EEPROM_Manager::getInstance().saveKnownMasterMac(knownMasterMac);
    }
  }

  if (planetopia::utils::MacAddress(lastSeenMasterMac) !=
          planetopia::utils::MacAddress(msg.originMacAddress) &&
      lastSeenMasterMac[0] != 0) {
    if (_dualMasterMode) {
      Logger::logln("MESH", "Two masters active (dual master mode)", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("MESH", "WARNING: Multiple masters detected!", LogLevel::LOG_WARN);
    }
  }
  memcpy(lastSeenMasterMac, msg.originMacAddress, 6);
  lastMasterBeaconReceivedMs = millis();

  uint8_t newDistance = msg.hopCount + 1;
  if (currentMaster.distance == 0xFF ||
      planetopia::utils::MacAddress(currentMaster.mac) !=
          planetopia::utils::MacAddress(msg.originMacAddress) ||
      newDistance < currentMaster.distance) {
    memcpy(currentMaster.mac, msg.originMacAddress, 6);
    currentMaster.distance = newDistance;
    memcpy(currentMaster.nextHop, msg.lastHopMacAddress, 6);
  }

  if (!isMaster) {
    // C10 fix: only relay if this beacon is newer than the last one we relayed
    bool isNewer = (msg.epochNum > lastRelayedEpoch) ||
                   (msg.epochNum == lastRelayedEpoch && msg.seqNum > lastRelayedSeqNum);
    if (!isNewer) {
      Logger::logln("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    lastRelayedEpoch = msg.epochNum;
    lastRelayedSeqNum = msg.seqNum;

    uint8_t jitterMs = static_cast<uint8_t>(esp_random() % planetopia::config::RELAY_JITTER_MAX_MS);
    relayPendingMsg = msg;
    relayPendingMsg.hopCount = newDistance;
    memcpy(relayPendingMsg.lastHopMacAddress, deviceMacAddress, 6);
    relayPendingAt = millis() + 10 + jitterMs;
    relayPending = true;
  }
}
```

- [ ] **Step 9: Run the five new tests to verify they pass**

```
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all 5 new dual-master tests PASS, all prior tests still PASS.

- [ ] **Step 10: Commit**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
git add main/project_config.h main/src/Mesh/Mesh.h main/src/Mesh/Mesh.cpp \
        tests/mocks/firmware_stubs.cpp tests/mocks/mesh_logic_impl.cpp \
        tests/unit/test_mesh_logic.cpp
git commit -m "feat: DUAL_MASTER_MODE flag — dual TOFU, accept both master MACs, suppress multi-master warning"
```
