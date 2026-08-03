# Phase 7: Bug Fixes & Firmware Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the JOIN_ACK rejection frame MAC field bug (spec §5.13) and decouple Mesh from Serial_Adapter via callback (spec §5.15).

**Architecture:** Two independent changes — a one-line server fix to send rejection frames to the correct MAC, and a firmware refactor that replaces a static call with a registered callback to eliminate a circular dependency.

**Tech Stack:** Go 1.22 (server), C++17 (firmware), GTest (firmware unit tests), nanopb/protobuf3.

## Pre-Implementation Audit

The following spec sections from §5 were already implemented before Phase 7 and require no changes:

| Spec | Status | Evidence |
|---|---|---|
| §5.5 Remove 3-chunk enrollment | **Done** | `Mesh.cpp:984-997` sends single `MESH_TYPE_ENROLLMENT`; `GTest EnrollmentTest::SendsSingleEspNowMessage` passes |
| §5.12 Online timeout 75s | **Done** | `api.go:267` `GetOnlineNodes(75 * time.Second)` already present |
| §5.14 Remove EEPROM wipe on WDT | **Done** | `main.ino:104-107` halts without `em.clearAll()` |
| §5.13 Firmware side (targetMacAddress) | **Done** | `Serial_Adapter.cpp:402` calls `enrollPeer(msg.targetMacAddress, …)`; `decodeMeshMessage` copies `pbMsg.targetMacAddress` at line 281 |

Only §5.13 server-side (rejection frame) and §5.15 remain.

## Global Constraints

- Go module root: `motionSensorServer/server/orchestrator/` — all Go paths relative to this
- Go test command: `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1`
- Firmware test command: `cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes && cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
- No `gh` commands. No `ben-clearscore` in git identity.
- Local git identity: `git config user.email "49689582+superbrobenji@users.noreply.github.com"` (set per-repo before committing)

---

## File Map

| File | Change |
|---|---|
| `mesh/server.go` | Fix `OriginMacAddress` → `TargetMacAddress` in `RejectEnrollment` |
| `mesh/server_enrollment_test.go` | Add `TestRejectEnrollment_SendsJoinAckToTargetMac` |
| `main/src/Mesh/Mesh.h` | Add `EnrollmentRelayFn` typedef, `setEnrollmentRelayFn()` declaration, `_enrollmentRelayFn` field, `drainPendingEnrollment()` private declaration |
| `main/src/Mesh/Mesh.cpp` | Remove `#include Serial_Adapter.h`, add `setEnrollmentRelayFn()` impl, extract drain to `drainPendingEnrollment()`, update `loop()` to call it |
| `tests/mocks/mesh_logic_impl.cpp` | Add `setEnrollmentRelayFn()` + `drainPendingEnrollment()` implementations (mirrors Mesh.cpp) |
| `tests/unit/test_mesh_logic.cpp` | Add `EnrollmentRelayCallbackTest` fixture + two tests |
| `main/main.ino` | Register `Serial_Adapter::relayEnrollmentToServer` as callback after `mesh.init()` |

---

### Task 1: Fix §5.13 — Rejection JOIN_ACK Uses TargetMacAddress

**Files:**
- Modify: `mesh/server.go:544-548`
- Test: `mesh/server_enrollment_test.go`

**Interfaces:**
- Consumes: `enrollTestNode(t, ms)` helper (lines 17-27 of `server_enrollment_test.go`) — returns `(macStr string, pubKey [32]byte)`, MAC is `{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}`
- Consumes: `decodeWrittenFrame(t, mockPort)` helper (lines 29-45) — decodes one frame from `mockPort.writeOffset`, returns `*MeshMessage`
- Produces: no new public API

- [ ] **Step 1: Write the failing test**

Add to `mesh/server_enrollment_test.go` after line 105:

```go
func TestRejectEnrollment_SendsJoinAckToTargetMac(t *testing.T) {
	ms := newTestMeshServer(t)
	mockPort := NewMockSerialPort()
	ms.serialComm = NewSerialComm(mockPort)

	macStr, _ := enrollTestNode(t, ms)

	if err := ms.RejectEnrollment(macStr); err != nil {
		t.Fatalf("RejectEnrollment returned error: %v", err)
	}

	msg := decodeWrittenFrame(t, mockPort)
	wantMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	if !bytes.Equal(msg.TargetMacAddress, wantMAC) {
		t.Errorf("TargetMacAddress = %x, want %x", msg.TargetMacAddress, wantMAC)
	}
	if len(msg.OriginMacAddress) != 0 {
		t.Errorf("OriginMacAddress should be absent in rejection frame, got %x", msg.OriginMacAddress)
	}
	if len(msg.PublicKey) != 0 {
		t.Errorf("PublicKey should be absent (rejection signal), got %x", msg.PublicKey)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestRejectEnrollment_SendsJoinAckToTargetMac
```

Expected: FAIL — `TargetMacAddress = [], want [aabbccddeeff]` (currently uses `OriginMacAddress`).

- [ ] **Step 3: Apply the fix in `mesh/server.go`**

Find `RejectEnrollment` (around line 542). Change:

```go
// BEFORE:
rejectMsg := &MeshMessage{
    MessageType:      MessageTypeJoinAck,
    OriginMacAddress: mac[:],
    // PublicKey intentionally absent — rejection signal
}
```

To:

```go
// AFTER:
rejectMsg := &MeshMessage{
    MessageType:      MessageTypeJoinAck,
    TargetMacAddress: mac[:],
    // PublicKey intentionally absent — rejection signal
}
```

- [ ] **Step 4: Run tests to verify they pass**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1
```

Expected: all tests PASS.

- [ ] **Step 5: Commit**

```bash
git add mesh/server.go mesh/server_enrollment_test.go
git commit -m "fix: rejection JOIN_ACK uses TargetMacAddress (spec §5.13)"
```

---

### Task 2: Decouple Mesh from Serial_Adapter via Callback (§5.15)

**Files:**
- Modify: `main/src/Mesh/Mesh.h`
- Modify: `main/src/Mesh/Mesh.cpp`
- Modify: `tests/mocks/mesh_logic_impl.cpp`
- Test: `tests/unit/test_mesh_logic.cpp`
- Modify: `main/main.ino`

**Interfaces:**
- Consumes: `Mesh::_pendingEnrollmentRelay`, `_pendingEnrollmentMac[6]`, `_pendingEnrollmentPubKey[32]` — existing private fields in Mesh.h:189-192
- Consumes: `Mesh::drainRecvQueue()` at Mesh.h:206 — existing private drain method, same pattern to follow
- Produces:
  - `typedef void (*EnrollmentRelayFn)(const uint8_t mac[6], const uint8_t pubKey[32])` — in `Mesh.h`
  - `void Mesh::setEnrollmentRelayFn(EnrollmentRelayFn fn)` — public method
  - `void Mesh::drainPendingEnrollment()` — private method; called from `loop()`

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_mesh_logic.cpp` after the existing `EnrollmentTest` fixture (after line 567):

```cpp
// ---- EnrollmentRelayCallbackTest ----

static const uint8_t* g_capturedMac = nullptr;
static const uint8_t* g_capturedKey = nullptr;

static void captureRelayFn(const uint8_t mac[6], const uint8_t pubKey[32]) {
  g_capturedMac = mac;
  g_capturedKey = pubKey;
}

class EnrollmentRelayCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_capturedMac = nullptr;
    g_capturedKey = nullptr;
    EEPROM.reset();
  }
};

TEST_F(EnrollmentRelayCallbackTest, DrainCallsRegisteredCallback) {
  Mesh mesh;
  mesh.isMaster = true;
  mesh.setEnrollmentRelayFn(captureRelayFn);

  static constexpr uint8_t kMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kKey[32] = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};

  memcpy(mesh._pendingEnrollmentMac, kMac, 6);
  memcpy(mesh._pendingEnrollmentPubKey, kKey, 32);
  mesh._pendingEnrollmentRelay = true;

  mesh.drainPendingEnrollment();

  EXPECT_FALSE(mesh._pendingEnrollmentRelay) << "flag must clear after drain";
  ASSERT_NE(g_capturedMac, nullptr) << "callback was not called";
  EXPECT_EQ(memcmp(g_capturedMac, kMac, 6), 0) << "wrong MAC passed to callback";
  EXPECT_EQ(memcmp(g_capturedKey, kKey, 32), 0) << "wrong pubKey passed to callback";
}

TEST_F(EnrollmentRelayCallbackTest, DrainWithNoCallbackClearsFlag) {
  Mesh mesh;
  mesh.isMaster = true;
  // No callback registered.

  mesh._pendingEnrollmentRelay = true;
  mesh.drainPendingEnrollment();

  EXPECT_FALSE(mesh._pendingEnrollmentRelay) << "flag must clear even with no callback";
  EXPECT_EQ(g_capturedMac, nullptr) << "callback must not fire when unregistered";
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
cmake -B build -S . && cmake --build build 2>&1 | head -30
```

Expected: compile error — `setEnrollmentRelayFn` and `drainPendingEnrollment` not declared.

- [ ] **Step 3: Update `main/src/Mesh/Mesh.h`**

Add after the `setIsMaster` public method block (find the public enrollment methods section around line 255):

```cpp
// Enrollment relay callback — registered by Serial_Adapter owner (main.ino).
// Called from loop() when a pending enrollment is ready to relay to the server.
typedef void (*EnrollmentRelayFn)(const uint8_t mac[6], const uint8_t pubKey[32]);
void setEnrollmentRelayFn(EnrollmentRelayFn fn);
```

Add in the private section (after `_pendingEnrollmentPubKey[32]{}` at line 192):

```cpp
EnrollmentRelayFn _enrollmentRelayFn = nullptr;
```

Add in the private methods section (after `void drainRecvQueue();` at line 206):

```cpp
void drainPendingEnrollment();
```

- [ ] **Step 4: Update `main/src/Mesh/Mesh.cpp`**

**4a.** Remove line 6:
```cpp
// DELETE THIS LINE:
#include "src/Adapter/Serial_Adapter/Serial_Adapter.h" // for relayEnrollmentToServer
```

**4b.** Add `setEnrollmentRelayFn` implementation. Place it near the other setter methods in Mesh.cpp:

```cpp
void Mesh::setEnrollmentRelayFn(EnrollmentRelayFn fn) {
  _enrollmentRelayFn = fn;
}
```

**4c.** Extract the enrollment drain into `drainPendingEnrollment()`. Find the loop drain block (around lines 1082-1087):

```cpp
// BEFORE (in loop()):
if (_pendingEnrollmentRelay) {
    _pendingEnrollmentRelay = false;
    planetopia::adapter::Serial_Adapter::relayEnrollmentToServer(_pendingEnrollmentMac,
                                                                 _pendingEnrollmentPubKey);
}
```

**4c-i.** Add the new method (place before `loop()`):

```cpp
void Mesh::drainPendingEnrollment() {
  if (!_pendingEnrollmentRelay) return;
  _pendingEnrollmentRelay = false;
  if (_enrollmentRelayFn) {
    _enrollmentRelayFn(_pendingEnrollmentMac, _pendingEnrollmentPubKey);
  }
}
```

**4c-ii.** Replace the old drain block in `loop()` with a single call:

```cpp
// AFTER (in loop()):
drainPendingEnrollment();
```

- [ ] **Step 5: Update `tests/mocks/mesh_logic_impl.cpp`**

The mock implementation mirrors Mesh.cpp methods for host compilation. Add at the end of the file (before the closing `}` namespace braces):

```cpp
void Mesh::setEnrollmentRelayFn(EnrollmentRelayFn fn) {
  _enrollmentRelayFn = fn;
}

void Mesh::drainPendingEnrollment() {
  if (!_pendingEnrollmentRelay) return;
  _pendingEnrollmentRelay = false;
  if (_enrollmentRelayFn) {
    _enrollmentRelayFn(_pendingEnrollmentMac, _pendingEnrollmentPubKey);
  }
}
```

- [ ] **Step 6: Register callback in `main/main.ino`**

After `mesh.init()` (line 230), add:

```cpp
mesh.setEnrollmentRelayFn(Serial_Adapter::relayEnrollmentToServer);
```

The full context:

```cpp
if (!mesh.init()) {
    // ... fatal error handling
}

mesh.setEnrollmentRelayFn(Serial_Adapter::relayEnrollmentToServer);

// Nodes must always receive — modem sleep drops ESP-NOW packets without AP sync
esp_wifi_set_ps(WIFI_PS_NONE);
```

- [ ] **Step 7: Build and run firmware tests**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests PASS, including the two new `EnrollmentRelayCallbackTest` tests.

- [ ] **Step 8: Verify Serial_Adapter.h is no longer included from Mesh.cpp**

```bash
grep -n "Serial_Adapter" /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes/main/src/Mesh/Mesh.cpp
```

Expected: no output.

- [ ] **Step 9: Commit**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
git add main/src/Mesh/Mesh.h main/src/Mesh/Mesh.cpp tests/mocks/mesh_logic_impl.cpp tests/unit/test_mesh_logic.cpp main/main.ino
git commit -m "refactor: decouple Mesh from Serial_Adapter via EnrollmentRelayFn callback (spec §5.15)"
```
