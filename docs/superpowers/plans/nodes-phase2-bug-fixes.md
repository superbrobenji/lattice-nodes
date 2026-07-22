# Phase 2 Bug Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix four production bugs across firmware and server — WDT EEPROM wipe, JOIN_ACK peer MAC field, TX power raw-frame bypass, and stale online-node timeout.

**Architecture:** Two repositories, two tasks. Task 1 touches only firmware (ESP32 Arduino/CMake project). Task 2 touches only the Go server. Each task is independently committable and reviewable. Changes are small (1–10 lines each) with targeted tests.

**Tech Stack:** C++17 / Arduino ESP32 (firmware), Go 1.26 (server), GoogleTest + CMake (firmware tests), `go test` (server tests), nanopb/protobuf3 (wire format).

## Global Constraints

- Firmware repo root: `/Users/benjamin.swanepoel/projects/personal/Planetopia-nodes`
- Server repo root: `/Users/benjamin.swanepoel/projects/personal/motionSensorServer`
- Both repos are on branch `feat/phase2-bug-fixes` off `main` — do NOT commit to `main`
- Firmware test command: `cmake -B tests/build -S tests && cmake --build tests/build && ctest --test-dir tests/build --output-on-failure`
- Server test command (run from `server/orchestrator/`): `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/...`
- Git identity for commits: local config is already set to `49689582+superbrobenji@users.noreply.github.com` — do not change it
- No new abstractions, no refactoring beyond what each fix requires
- `Serial_Adapter.cpp` is NOT compiled in the firmware unit test build (it is hardware-dependent) — firmware tests only cover Mesh logic; behavioral correctness of Serial_Adapter changes is verified by the corresponding server-side test

---

### Task 1: Firmware bug fixes — WDT loop + JOIN_ACK peer MAC

Two one-line fixes in firmware. Neither has a dedicated unit test (Serial_Adapter is excluded from the test build); the passing test suite confirms no regression.

**Files:**
- Modify: `main/main.ino` (lines 104–108)
- Modify: `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` (line 403)

**Interfaces:**
- Consumes: nothing from other tasks
- Produces: nothing consumed by other tasks

---

- [ ] **Step 1: Write the WDT fix**

In `main/main.ino` around line 104, find this block:

```cpp
      Serial.println("[BOOT] WDT loop detected — clearing EEPROM and halting");
      em.clearAll();
      while (true) { delay(1000); }
```

Replace with:

```cpp
      Serial.println("[BOOT] WDT loop detected — halting. Manual reset required.");
      while (true) { delay(1000); }
```

The `clearAll()` call was destroying enrollment state and peer keys on every WDT recovery loop, forcing re-enrollment after every firmware crash. Physical factory-reset (button sequence) is the intentional path for wiping EEPROM.

- [ ] **Step 2: Write the JOIN_ACK peer MAC fix**

In `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` around line 403, find:

```cpp
        meshInstance->enrollPeer(msg.originMacAddress, msg.enrollmentPublicKey);
```

Replace with:

```cpp
        meshInstance->enrollPeer(msg.targetMacAddress, msg.enrollmentPublicKey);
```

The peer being enrolled is the node receiving the JOIN_ACK — that is the *target* of the message, not the origin. The server populates `targetMacAddress` with the enrolling node's MAC; `originMacAddress` was populated with the master's MAC (wrong peer added).

- [ ] **Step 3: Run firmware tests**

```bash
cmake -B tests/build -S tests && cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: all 47 tests pass, 0 failures.

- [ ] **Step 4: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes \
  add main/main.ino main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes \
  commit -m "fix(firmware): remove EEPROM wipe on WDT loop; use targetMacAddress in enrollPeer"
```

---

### Task 2: Server bug fixes — TX power protobuf, JOIN_ACK TargetMacAddress, online timeout

Three fixes in two Go files. New tests for the TX power and JOIN_ACK changes; the timeout change is covered by a new threshold boundary test on `NodeRegistry`.

**Files:**
- Modify: `server/orchestrator/mesh/server.go` (two locations: `SetTxPowerPreset` ~line 584, `ApproveEnrollment` ~line 436)
- Modify: `server/orchestrator/mesh/api.go` (line 241)
- Modify (tests): `server/orchestrator/mesh/mesh_test.go` — add `TestGetOnlineNodes_ThresholdBoundary`
- Modify (tests): `server/orchestrator/mesh/server_enrollment_test.go` — update `TestApproveEnrollment_SendsJoinAckWithPubKey`
- Create (tests): `server/orchestrator/mesh/server_test.go` — add `TestSetTxPowerPreset_SendsProtoFrame`

**Interfaces:**
- Consumes: nothing from Task 1
- Produces: nothing consumed downstream

---

- [ ] **Step 1: Write the failing TX power test**

Create `server/orchestrator/mesh/server_test.go`:

```go
package mesh

import (
	"encoding/binary"
	"testing"

	"google.golang.org/protobuf/proto"
)

func TestSetTxPowerPreset_SendsProtoFrame(t *testing.T) {
	ms := newTestMeshServer(t)
	mockPort := NewMockSerialPort()
	ms.serialComm = NewSerialComm(mockPort)

	if err := ms.SetTxPowerPreset(1); err != nil {
		t.Fatalf("SetTxPowerPreset(1) returned error: %v", err)
	}

	data := mockPort.GetWrittenData()
	if len(data) < 2 {
		t.Fatalf("no frame written: only %d bytes", len(data))
	}
	length := int(binary.LittleEndian.Uint16(data[:2]))
	if len(data) < 2+length {
		t.Fatalf("frame truncated: need %d bytes after header, have %d", length, len(data)-2)
	}

	var msg MeshMessage
	if err := proto.Unmarshal(data[2:2+length], &msg); err != nil {
		t.Fatalf("frame is not valid protobuf: %v — WriteRaw was used instead of WriteFrame", err)
	}

	if msg.MessageType != MessageTypeAdapterData {
		t.Errorf("MessageType = %d, want %d (MessageTypeAdapterData)", msg.MessageType, MessageTypeAdapterData)
	}
	if msg.DataType != AdapterTypeSerial {
		t.Errorf("DataType = %d, want %d (AdapterTypeSerial)", msg.DataType, AdapterTypeSerial)
	}
	if len(msg.Data) == 0 || msg.Data[0] != OpTxPowerSet {
		t.Errorf("Data[0] = %d, want %d (OpTxPowerSet)", msg.Data[0], OpTxPowerSet)
	}
	if len(msg.Data) < 2 || msg.Data[1] != 1 {
		t.Errorf("Data[1] = %d, want 1 (preset)", msg.Data[1])
	}
}

func TestSetTxPowerPreset_InvalidPreset_ReturnsError(t *testing.T) {
	ms := newTestMeshServer(t)
	if err := ms.SetTxPowerPreset(3); err == nil {
		t.Error("expected error for preset=3, got nil")
	}
}
```

- [ ] **Step 2: Run to verify TX power test fails**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestSetTxPowerPreset -v
```

Expected: `TestSetTxPowerPreset_SendsProtoFrame` FAIL with "frame is not valid protobuf" (because `WriteRaw` produces a non-protobuf payload). `TestSetTxPowerPreset_InvalidPreset_ReturnsError` PASS (validation already exists).

- [ ] **Step 3: Fix SetTxPowerPreset to use WriteFrame**

In `server/orchestrator/mesh/server.go`, find `SetTxPowerPreset` (~line 575). Replace the entire payload/frame/WriteRaw block:

```go
// Before:
	// Frame: [2-byte LE length][A1][preset]
	payload := []byte{OpTxPowerSet, preset}
	header := []byte{byte(len(payload) & 0xFF), byte((len(payload) >> 8) & 0xFF)}
	frame := append(header, payload...)

	if err := ms.serialComm.WriteRaw(frame); err != nil {
		return fmt.Errorf("failed to send TX power preset: %w", err)
	}
```

```go
// After:
	payload := make([]byte, MaxDataLength)
	payload[0] = OpTxPowerSet
	payload[1] = preset
	msg := &MeshMessage{
		MessageType: MessageTypeAdapterData,
		DataType:    AdapterTypeSerial,
		Data:        payload,
	}
	if err := ms.serialComm.WriteFrame(msg); err != nil {
		return fmt.Errorf("failed to send TX power preset: %w", err)
	}
```

- [ ] **Step 4: Run to verify TX power test passes**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestSetTxPowerPreset -v
```

Expected: both TX power tests PASS.

- [ ] **Step 5: Write the failing JOIN_ACK TargetMacAddress assertion**

In `server/orchestrator/mesh/server_enrollment_test.go`, find `TestApproveEnrollment_SendsJoinAckWithPubKey` (around line 45). Add these assertions at the end of the test, after the existing `PublicKey` check:

```go
	// TargetMacAddress must carry the enrolling node's MAC — not OriginMacAddress
	wantMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF} // from enrollTestNode helper
	if !bytes.Equal(msg.TargetMacAddress, wantMAC) {
		t.Errorf("TargetMacAddress = %x, want %x", msg.TargetMacAddress, wantMAC)
	}
	if len(msg.OriginMacAddress) != 0 {
		t.Errorf("OriginMacAddress should be absent, got %x", msg.OriginMacAddress)
	}
```

Add `"bytes"` to the import block if not already present.

- [ ] **Step 6: Run to verify JOIN_ACK test fails**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestApproveEnrollment_SendsJoinAckWithPubKey -v
```

Expected: FAIL — `TargetMacAddress` is empty/nil because `ApproveEnrollment` currently sets `OriginMacAddress`.

- [ ] **Step 7: Fix ApproveEnrollment to use TargetMacAddress**

In `server/orchestrator/mesh/server.go`, find `ApproveEnrollment` (~line 434). Change:

```go
// Before:
		ackMsg := &MeshMessage{
			MessageType:      MessageTypeJoinAck,
			OriginMacAddress: node.MAC[:],
			PublicKey:        node.PublicKey[:],
		}
```

```go
// After:
		ackMsg := &MeshMessage{
			MessageType:      MessageTypeJoinAck,
			TargetMacAddress: node.MAC[:],
			PublicKey:        node.PublicKey[:],
		}
```

The JOIN_ACK is a message FROM the master TO the enrolling node. The node is the *target*. Firmware's `processJoinAck` checks `msg.targetMacAddress` to determine if the ACK is for this node.

- [ ] **Step 8: Run to verify JOIN_ACK test passes**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestApproveEnrollment -v
```

Expected: all `TestApproveEnrollment_*` tests PASS.

- [ ] **Step 9: Write the failing online timeout threshold test**

In `server/orchestrator/mesh/mesh_test.go`, add after `TestNodeRegistry` (around line 178):

```go
func TestGetOnlineNodes_ThresholdBoundary(t *testing.T) {
	registry := NewNodeRegistry()
	mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	macStr := macToString(mac)

	registry.UpdateNode(mac, AdapterTypePIR, 1000, 1)

	// Backdate LastSeen to 45 seconds ago: within 75s threshold but outside 30s threshold
	registry.mu.Lock()
	registry.nodes[macStr].LastSeen = time.Now().Add(-45 * time.Second)
	registry.mu.Unlock()

	if got := registry.GetOnlineNodes(30 * time.Second); len(got) != 0 {
		t.Errorf("GetOnlineNodes(30s): expected 0 nodes for a 45s-old node, got %d", len(got))
	}
	if got := registry.GetOnlineNodes(75 * time.Second); len(got) != 1 {
		t.Errorf("GetOnlineNodes(75s): expected 1 node for a 45s-old node, got %d", len(got))
	}
}
```

Make sure `"time"` is already in the imports of `mesh_test.go` (it is, from existing tests).

- [ ] **Step 10: Run to verify threshold test passes**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestGetOnlineNodes_ThresholdBoundary -v
```

Expected: PASS (the test doesn't test `api.go`'s hardcoded value yet — it documents the correct threshold behavior).

- [ ] **Step 11: Fix the hardcoded 30s timeout in api.go**

In `server/orchestrator/mesh/api.go` line 241, change:

```go
// Before:
	onlineNodes := registry.GetOnlineNodes(30 * time.Second) // 30 second timeout
```

```go
// After:
	onlineNodes := registry.GetOnlineNodes(75 * time.Second) // 2.5× the 30s health interval — single missed report no longer marks offline
```

- [ ] **Step 12: Run full server test suite**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/...
```

Expected: all tests pass, 0 failures.

- [ ] **Step 13: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer \
  add server/orchestrator/mesh/server.go \
      server/orchestrator/mesh/api.go \
      server/orchestrator/mesh/mesh_test.go \
      server/orchestrator/mesh/server_enrollment_test.go \
      server/orchestrator/mesh/server_test.go
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer \
  commit -m "fix(server): TX power via WriteFrame, JOIN_ACK TargetMacAddress, online timeout 75s"
```
