# Phase 8: Node Hotswap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow a failed node to be physically replaced by a new ESP32 that inherits the original's logical ID, name, zone, and adapter type — without requiring the operator to re-enter any of that information.

**Architecture:** Server-only change. The new ESP32 enrolls normally (firmware is unchanged). When the operator approves with an existing `nodeId`, `ApproveEnrollment` detects the hotswap, inherits all unspecified fields from the old node, marks the old registry entry as `"replaced"`, and sends `OP_CONFIG_SET` with the inherited adapter type. The old entry's `nodeId` is cleared so it no longer appears in API responses (the existing `NodeID > 0` filter in `v1GetNodes` handles exclusion automatically).

**Tech Stack:** Go 1.22, gorilla/mux, nanopb/protobuf3 serial frames.

## Global Constraints

- Go module root: `motionSensorServer/server/orchestrator/` — all paths relative to this
- Go test command: `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1`
- `AdapterTypeUnknown int32 = -1` (not 0 — `AdapterTypePIR = 0` is a valid real type)
- MAC keys in `NodeRegistry.nodes` are colon-separated lowercase (from `macToString`: `"aa:bb:cc:dd:ee:ff"`)
- MAC strings in `authRegistry` / URL paths are no-colon lowercase (from `enrollTestNode`: `"aabbccddeeff"`)
- No `gh` commands. No `ben-clearscore` in git identity.
- Set per-repo before committing: `git config user.email "49689582+superbrobenji@users.noreply.github.com"` and `git config user.name "superbrobenji"`

---

## File Map

| File | Change |
|---|---|
| `mesh/node_registry.go` | Add `Status`/`ReplacedBy` to `NodeInfo` + `persistedNode`; update `Persist`/`Load`; add `MarkReplaced()` |
| `mesh/mesh_test.go` | Add 3 tests for `MarkReplaced` |
| `mesh/server.go` | Add hotswap detection + field inheritance + `MarkReplaced` call + OP_CONFIG_SET send in `ApproveEnrollment`; add `bytes` import |
| `mesh/server_enrollment_test.go` | Add `enrollTestNodeWithMAC` helper + 3 hotswap tests for `ApproveEnrollment` |
| `mesh/api_v1_nodes_test.go` | Add 1 integration test: hotswap approval → old node excluded from GET /api/v1/nodes |

---

### Task 1: Add Status/ReplacedBy fields and MarkReplaced to NodeRegistry

**Files:**
- Modify: `mesh/node_registry.go`
- Test: `mesh/mesh_test.go`

**Interfaces:**
- Produces: `func (nr *NodeRegistry) MarkReplaced(mac []byte, replacedByMACStr string)` — sets `Status = "replaced"`, `ReplacedBy = replacedByMACStr`, `NodeID = 0` on the node with the given MAC; no-op if MAC not found

- [ ] **Step 1: Write three failing tests in `mesh/mesh_test.go`**

Add after the existing node registry tests (search for `TestGetOnlineNodes` to find the right area):

```go
func TestMarkReplaced_SetsStatusAndClearsNodeID(t *testing.T) {
	nr := NewNodeRegistry()
	mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	nr.AssignNode(mac, 7, "entrance-left", "lobby")

	nr.MarkReplaced(mac, "11:22:33:44:55:66")

	node, ok := nr.GetNode(mac)
	if !ok {
		t.Fatal("node must still exist in registry after MarkReplaced")
	}
	if node.Status != "replaced" {
		t.Errorf("Status = %q, want %q", node.Status, "replaced")
	}
	if node.ReplacedBy != "11:22:33:44:55:66" {
		t.Errorf("ReplacedBy = %q, want %q", node.ReplacedBy, "11:22:33:44:55:66")
	}
	if node.NodeID != 0 {
		t.Errorf("NodeID = %d, want 0 after replacement", node.NodeID)
	}
}

func TestMarkReplaced_ReplacedNodeNotReturnedByGetNodeByID(t *testing.T) {
	nr := NewNodeRegistry()
	mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	nr.AssignNode(mac, 7, "entrance-left", "lobby")

	nr.MarkReplaced(mac, "11:22:33:44:55:66")

	_, ok := nr.GetNodeByID(7)
	if ok {
		t.Error("GetNodeByID(7) must not return a replaced node")
	}
}

func TestMarkReplaced_PersistsAndLoadsCorrectly(t *testing.T) {
	nr := NewNodeRegistry()
	mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	nr.AssignNode(mac, 7, "entrance-left", "lobby")
	nr.MarkReplaced(mac, "11:22:33:44:55:66")

	path := t.TempDir() + "/nodes.json"
	if err := nr.Persist(path); err != nil {
		t.Fatalf("Persist: %v", err)
	}

	nr2 := NewNodeRegistry()
	if err := nr2.Load(path); err != nil {
		t.Fatalf("Load: %v", err)
	}

	node, ok := nr2.GetNode(mac)
	if !ok {
		t.Fatal("replaced node must survive Persist/Load round-trip")
	}
	if node.Status != "replaced" {
		t.Errorf("Status after load = %q, want %q", node.Status, "replaced")
	}
	if node.ReplacedBy != "11:22:33:44:55:66" {
		t.Errorf("ReplacedBy after load = %q, want %q", node.ReplacedBy, "11:22:33:44:55:66")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestMarkReplaced
```

Expected: compile error — `MarkReplaced` not declared.

- [ ] **Step 3: Add Status and ReplacedBy fields to NodeInfo and persistedNode in `mesh/node_registry.go`**

In `NodeInfo` (around line 15), add two new fields after `Zone`:

```go
type NodeInfo struct {
	MAC        []byte    `json:"mac"`
	MACString  string    `json:"macString"`
	AdapterType int32    `json:"adapterType"`
	Uptime     uint32    `json:"uptime"`
	LastSeen   time.Time `json:"lastSeen"`
	HopCount   uint32    `json:"hopCount"`
	NodeID     uint8     `json:"nodeId,omitempty"`
	Name       string    `json:"name,omitempty"`
	Zone       string    `json:"zone,omitempty"`
	Status     string    `json:"status,omitempty"`
	ReplacedBy string    `json:"replacedBy,omitempty"`
}
```

In `persistedNode` (around line 211), add the same two fields after `Zone`:

```go
type persistedNode struct {
	MAC         string    `json:"mac"`
	AdapterType int32     `json:"adapterType"`
	Uptime      uint32    `json:"uptime"`
	LastSeen    time.Time `json:"lastSeen"`
	HopCount    uint32    `json:"hopCount"`
	NodeID      uint8     `json:"nodeId,omitempty"`
	Name        string    `json:"name,omitempty"`
	Zone        string    `json:"zone,omitempty"`
	Status      string    `json:"status,omitempty"`
	ReplacedBy  string    `json:"replacedBy,omitempty"`
}
```

- [ ] **Step 4: Update Persist to include the new fields**

In `Persist` (around line 223), update the `persistedNode` literal inside the loop:

```go
entries = append(entries, persistedNode{
    MAC:         n.MACString,
    AdapterType: n.AdapterType,
    Uptime:      n.Uptime,
    LastSeen:    n.LastSeen,
    HopCount:    n.HopCount,
    NodeID:      n.NodeID,
    Name:        n.Name,
    Zone:        n.Zone,
    Status:      n.Status,
    ReplacedBy:  n.ReplacedBy,
})
```

- [ ] **Step 5: Update Load to populate the new fields**

In `Load` (around line 273), update the `NodeInfo` literal inside the loop:

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
    Status:      e.Status,
    ReplacedBy:  e.ReplacedBy,
}
```

- [ ] **Step 6: Add the MarkReplaced method to `mesh/node_registry.go`**

Add after `RemoveNode`:

```go
// MarkReplaced marks a node as replaced by a new MAC address.
// Sets Status = "replaced", ReplacedBy = replacedByMACStr, NodeID = 0
// so the entry is preserved for audit but excluded from active-node queries.
func (nr *NodeRegistry) MarkReplaced(mac []byte, replacedByMACStr string) {
	macStr := macToString(mac)
	nr.mu.Lock()
	defer nr.mu.Unlock()
	node, ok := nr.nodes[macStr]
	if !ok {
		return
	}
	node.Status = "replaced"
	node.ReplacedBy = replacedByMACStr
	node.NodeID = 0
}
```

- [ ] **Step 7: Run tests to verify they pass**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run TestMarkReplaced
```

Expected: all 3 tests PASS.

- [ ] **Step 8: Run full suite to check for regressions**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1
```

Expected: all tests PASS.

- [ ] **Step 9: Commit**

```bash
git add mesh/node_registry.go mesh/mesh_test.go
git commit -m "feat: add Status/ReplacedBy to NodeInfo and MarkReplaced method"
```

---

### Task 2: Hotswap detection and field inheritance in ApproveEnrollment

**Files:**
- Modify: `mesh/server.go`
- Test: `mesh/server_enrollment_test.go`
- Test: `mesh/api_v1_nodes_test.go`

**Interfaces:**
- Consumes (from Task 1): `func (nr *NodeRegistry) MarkReplaced(mac []byte, replacedByMACStr string)`
- Consumes: existing `ApproveEnrollment(macStr string, params ApprovalParams) error` — same signature, new behavior when `params.NodeID > 0` and an existing node has that ID
- Consumes: `ms.messageBuilder.BuildConfigSetMessage(targetMAC []byte, adapterType int32) (*MeshMessage, error)` — already used in codebase
- Consumes: `enrollTestNode(t, ms)` helper — returns `("aabbccddeeff", pubKey)`, MAC `{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}`
- Consumes: `decodeWrittenFrame(t, mockPort)` helper — decodes one frame from `mockPort.writeOffset`

- [ ] **Step 1: Add enrollTestNodeWithMAC helper to `mesh/server_enrollment_test.go`**

Add after the existing `enrollTestNode` helper (line 27):

```go
// enrollTestNodeWithMAC adds a pending enrollment for the given MAC address.
func enrollTestNodeWithMAC(t *testing.T, ms *MeshServer, mac [6]byte) (macStr string, pubKey [32]byte) {
	t.Helper()
	for i := range pubKey {
		pubKey[i] = byte(i + 1)
	}
	if err := ms.authRegistry.AddPending(mac, pubKey); err != nil {
		t.Fatalf("AddPending failed: %v", err)
	}
	return fmt.Sprintf("%02x%02x%02x%02x%02x%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]), pubKey
}
```

This produces no-colon MAC strings (e.g. `"112233445566"`) matching the format expected by `ApproveEnrollment` / `authRegistry`.

Also add `"fmt"` to the import block if not already present.

- [ ] **Step 2: Write three failing tests in `mesh/server_enrollment_test.go`**

Add after line 105:

```go
func TestApproveEnrollment_HotswapInheritsNameZoneAndMarksReplaced(t *testing.T) {
	ms := newTestMeshServer(t)

	// Old node: assigned with identity, adapter type known from health report
	oldMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	ms.nodeRegistry.AssignNode(oldMAC, 7, "entrance-left", "lobby")
	ms.nodeRegistry.UpdateNode(oldMAC, AdapterTypePIR, 3600, 1)

	// New node sends enrollment request
	newMacStr, _ := enrollTestNodeWithMAC(t, ms, [6]byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66})

	// Approve with same nodeId, no explicit overrides
	if err := ms.ApproveEnrollment(newMacStr, ApprovalParams{NodeID: 7}); err != nil {
		t.Fatalf("ApproveEnrollment: %v", err)
	}

	// New node should inherit name and zone from old node
	newNode, ok := ms.nodeRegistry.GetNodeByID(7)
	if !ok {
		t.Fatal("GetNodeByID(7) must return the new node after hotswap")
	}
	if newNode.Name != "entrance-left" {
		t.Errorf("Name = %q, want %q (inherited from old node)", newNode.Name, "entrance-left")
	}
	if newNode.Zone != "lobby" {
		t.Errorf("Zone = %q, want %q (inherited from old node)", newNode.Zone, "lobby")
	}

	// Old node must be marked replaced and its NodeID cleared
	oldNode, ok := ms.nodeRegistry.GetNode(oldMAC)
	if !ok {
		t.Fatal("old node must still exist in registry after hotswap")
	}
	if oldNode.Status != "replaced" {
		t.Errorf("old node Status = %q, want %q", oldNode.Status, "replaced")
	}
	if oldNode.NodeID != 0 {
		t.Errorf("old node NodeID = %d, want 0 after replacement", oldNode.NodeID)
	}
}

func TestApproveEnrollment_HotswapSendsConfigSet(t *testing.T) {
	ms := newTestMeshServer(t)
	mockPort := NewMockSerialPort()
	ms.serialComm = NewSerialComm(mockPort)

	// Old node with PIR adapter type
	oldMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	ms.nodeRegistry.AssignNode(oldMAC, 7, "entrance-left", "lobby")
	ms.nodeRegistry.UpdateNode(oldMAC, AdapterTypePIR, 3600, 1)

	newMacStr, _ := enrollTestNodeWithMAC(t, ms, [6]byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66})

	if err := ms.ApproveEnrollment(newMacStr, ApprovalParams{NodeID: 7}); err != nil {
		t.Fatalf("ApproveEnrollment: %v", err)
	}

	_ = decodeWrittenFrame(t, mockPort) // JOIN_ACK
	_ = decodeWrittenFrame(t, mockPort) // OP_NODE_ID_SET
	configMsg := decodeWrittenFrame(t, mockPort) // OP_CONFIG_SET

	if len(configMsg.Data) == 0 || configMsg.Data[0] != byte(OpConfigSet) {
		t.Errorf("3rd frame opcode = 0x%02x, want 0x%02x (OP_CONFIG_SET)",
			configMsg.Data[0], OpConfigSet)
	}
	if configMsg.Data[7] != byte(AdapterTypePIR) {
		t.Errorf("OP_CONFIG_SET adapter type = %d, want %d (AdapterTypePIR)",
			configMsg.Data[7], byte(AdapterTypePIR))
	}
}

func TestApproveEnrollment_HotswapExplicitOverrideNotInherited(t *testing.T) {
	ms := newTestMeshServer(t)

	// Old node with known name, zone, type
	oldMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	ms.nodeRegistry.AssignNode(oldMAC, 7, "entrance-left", "lobby")
	ms.nodeRegistry.UpdateNode(oldMAC, AdapterTypePIR, 3600, 1)

	newMacStr, _ := enrollTestNodeWithMAC(t, ms, [6]byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66})

	// Explicit overrides provided — should NOT inherit from old node
	if err := ms.ApproveEnrollment(newMacStr, ApprovalParams{
		NodeID:         7,
		Name:           "stage-right",
		Zone:           "stage",
		AdapterTypeStr: "led",
	}); err != nil {
		t.Fatalf("ApproveEnrollment: %v", err)
	}

	newNode, ok := ms.nodeRegistry.GetNodeByID(7)
	if !ok {
		t.Fatal("GetNodeByID(7) must return new node")
	}
	if newNode.Name != "stage-right" {
		t.Errorf("Name = %q, want %q (explicit override)", newNode.Name, "stage-right")
	}
	if newNode.Zone != "stage" {
		t.Errorf("Zone = %q, want %q (explicit override)", newNode.Zone, "stage")
	}
}
```

- [ ] **Step 3: Write the API integration test in `mesh/api_v1_nodes_test.go`**

Add after the existing tests:

```go
func TestV1Nodes_Hotswap_OldNodeExcludedNewNodePresent(t *testing.T) {
	api, ms := newV1TestServer(t)

	// Old node enrolled and assigned
	oldMAC := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
	ms.nodeRegistry.AssignNode(oldMAC, 7, "entrance-left", "lobby")
	ms.nodeRegistry.UpdateNode(oldMAC, AdapterTypePIR, 3600, 1)

	// New node sends enrollment
	newMAC := [6]byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
	var newPubKey [32]byte
	for i := range newPubKey {
		newPubKey[i] = byte(i + 1)
	}
	if err := ms.authRegistry.AddPending(newMAC, newPubKey); err != nil {
		t.Fatalf("AddPending: %v", err)
	}

	// Approve hotswap via API
	w := v1Request(t, api, "POST", "/api/v1/enrollments/112233445566/approve",
		map[string]interface{}{"nodeId": 7})
	if w.Code != http.StatusOK {
		t.Fatalf("approve returned %d, want 200", w.Code)
	}

	// GET /api/v1/nodes — must return exactly one node with id=7
	w = v1Request(t, api, "GET", "/api/v1/nodes", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/nodes returned %d", w.Code)
	}
	var resp APIResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data, _ := json.Marshal(resp.Data)
	var nodes []NodeV1
	if err := json.Unmarshal(data, &nodes); err != nil {
		t.Fatalf("unmarshal nodes: %v", err)
	}
	if len(nodes) != 1 {
		t.Fatalf("got %d nodes, want 1 (replaced node must be excluded)", len(nodes))
	}
	if nodes[0].ID != 7 {
		t.Errorf("node ID = %d, want 7", nodes[0].ID)
	}
	if nodes[0].Name != "entrance-left" {
		t.Errorf("Name = %q, want %q (inherited)", nodes[0].Name, "entrance-left")
	}
}
```

- [ ] **Step 4: Run tests to verify they fail**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run "TestApproveEnrollment_Hotswap|TestV1Nodes_Hotswap"
```

Expected: all FAIL — no hotswap logic in `ApproveEnrollment` yet.

- [ ] **Step 5: Add bytes import to `mesh/server.go`**

In the import block, add `"bytes"`:

```go
import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"sync"
	"time"

	EventStore "github.com/superbrobenji/motionServer/eventStore"
	"github.com/superbrobenji/motionServer/nodeauth"
	"go.bug.st/serial"
)
```

- [ ] **Step 6: Add hotswap detection and inheritance in ApproveEnrollment in `mesh/server.go`**

Find `ApproveEnrollment` (around line 465). Replace the section between `node, err := ms.authRegistry.Approve(macStr)` and `ms.nodeRegistry.AssignNode(...)` with this (the full replacement for the nodeId resolution + new hotswap block):

```go
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

	// Hotswap detection: explicit nodeId provided and an existing node already owns it.
	// Inherit unspecified fields from the old node; the old entry is marked replaced.
	var hotswapOldMAC []byte
	var inheritedAdapterType int32 = AdapterTypeUnknown // sentinel: no inheritance
	if params.NodeID > 0 {
		if oldNode, ok := ms.nodeRegistry.GetNodeByID(params.NodeID); ok &&
			!bytes.Equal(oldNode.MAC, node.MAC[:]) {
			hotswapOldMAC = oldNode.MAC
			if params.Name == "" {
				params.Name = oldNode.Name
			}
			if params.Zone == "" {
				params.Zone = oldNode.Zone
			}
			if params.AdapterTypeStr == "" && oldNode.AdapterType != AdapterTypeUnknown {
				inheritedAdapterType = oldNode.AdapterType
			}
		}
	}

	// Assign new node in registry (creates entry if first seen)
	ms.nodeRegistry.AssignNode(node.MAC[:], nodeId, params.Name, params.Zone)

	// Mark old node replaced after new node is assigned (ensures GetNodeByID uniqueness)
	if hotswapOldMAC != nil {
		ms.nodeRegistry.MarkReplaced(hotswapOldMAC, macToString(node.MAC[:]))
	}

	if registryNode, ok := ms.nodeRegistry.GetNode(node.MAC[:]); ok {
		typeStr := params.AdapterTypeStr
		if typeStr == "" {
			typeStr = "unknown"
		}
		ms.publishEnrolledEvent(registryNode, typeStr)
	}

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
			payload[0] = OpNodeIdSet
			copy(payload[1:7], node.MAC[:])
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

		// Send OP_CONFIG_SET when adapter type was inherited from old node
		if inheritedAdapterType != AdapterTypeUnknown {
			configMsg, buildErr := ms.messageBuilder.BuildConfigSetMessage(node.MAC[:], inheritedAdapterType)
			if buildErr != nil {
				slog.Warn("Failed to build OP_CONFIG_SET for hotswap", "mac", macStr, "error", buildErr)
			} else if err := ms.serialComm.WriteFrame(configMsg); err != nil {
				slog.Warn("Failed to send OP_CONFIG_SET on hotswap", "mac", macStr, "error", err)
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

- [ ] **Step 7: Run the new tests to verify they pass**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 -run "TestApproveEnrollment_Hotswap|TestV1Nodes_Hotswap"
```

Expected: all 4 tests PASS.

- [ ] **Step 8: Run full suite to check for regressions**

```
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1
```

Expected: all tests PASS.

- [ ] **Step 9: Commit**

```bash
git add mesh/server.go mesh/server_enrollment_test.go mesh/api_v1_nodes_test.go
git commit -m "feat: hotswap detection in ApproveEnrollment — inherit fields, mark replaced, send OP_CONFIG_SET"
```
