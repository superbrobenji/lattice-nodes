# Phase 6 — Artist API (/api/v1/) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a clean JSON abstraction layer at `/api/v1/` that hides mesh internals (MAC addresses, adapter type integers, opcodes, protobuf) from artists.

**Architecture:** Four sequential server-only tasks: (1) EventBroker + NodeRegistry helpers + MeshServer event wiring as the pub/sub foundation, (2) ZoneRegistry + `/api/v1/zones` CRUD, (3) `/api/v1/nodes` CRUD + command endpoints with type translation, (4) SSE event stream + `/api/v1/events`, `/api/v1/status`, `/api/v1/enrollments`. All existing routes remain untouched.

**Tech Stack:** Go 1.22, `github.com/gorilla/mux`, `text/event-stream` SSE, JSON file persistence

## Global Constraints

- All new routes under `/api/v1/` prefix; existing `/nodes`, `/api/enrollments/`, `/health/request`, etc. stay at their current paths
- Same `AuthMiddleware(apiKey)` Bearer token protection on all `/api/v1/` routes
- Type mapping (exact): `"pir"` ↔ `AdapterTypePIR (0)`, `"led"` ↔ `AdapterTypeLED (2)`, `"serial"` ↔ `AdapterTypeSerial (3)`, anything else → `"unknown"` (reading) / 400 (writing)
- Node external ID = `NodeID uint8` (1–255); never MAC in `/api/v1/nodes/{id}` routes
- Online threshold = 75 seconds: `time.Since(node.LastSeen) <= 75*time.Second`
- SSE event types (exact strings): `"motion"`, `"node_online"`, `"node_offline"`, `"enrolled"`, `"health"`
- SSE wire format per event: `"event: <type>\ndata: <json>\n\n"` (standard SSE; two trailing newlines)
- Zone ID = zone name lowercased, spaces replaced with hyphens (`"Main Hall"` → `"main-hall"`); enforced on creation; used in URL paths
- `EventBroker` client channel buffer size = 32 events; dropped (non-blocking send) when full
- `healthTimeout` on `MeshServer` = 75s (existing field — do not change)
- Go test command: `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1` from `motionSensorServer/server/orchestrator/`
- Repo: `motionSensorServer`, branch `feat/phase6-artist-api`
- No `gh` CLI; commit author local config already set to `49689582+superbrobenji@users.noreply.github.com`

---

### Task 1: EventBroker + NodeRegistry helpers + MeshServer event wiring

**Files:**
- Create: `server/orchestrator/mesh/event_broker.go`
- Modify: `server/orchestrator/mesh/node_registry.go` — add `GetNodeByID`, `GetNodesByZone`
- Modify: `server/orchestrator/mesh/server.go` — add `eventBroker`, `nodeOnlineState`, `GetEventBroker()`, `GetHealthTimeout()`, `SendNodeData()`, offline detector goroutine, publish calls in message handlers

**Interfaces:**
- Produces: `NewEventBroker() *EventBroker`
- Produces: `(*EventBroker).Subscribe() chan Event`
- Produces: `(*EventBroker).Unsubscribe(ch chan Event)`
- Produces: `(*EventBroker).Publish(e Event)`
- Produces: `Event{Type EventType; Data json.RawMessage; Timestamp time.Time}`
- Produces: `EventType` constants: `EventMotion`, `EventNodeOnline`, `EventNodeOffline`, `EventEnrolled`, `EventHealth`
- Produces: `(*NodeRegistry).GetNodeByID(nodeId uint8) (*NodeInfo, bool)`
- Produces: `(*NodeRegistry).GetNodesByZone(zone string) []*NodeInfo`
- Produces: `(*MeshServer).GetEventBroker() *EventBroker`
- Produces: `(*MeshServer).GetHealthTimeout() time.Duration`
- Produces: `(*MeshServer).SendNodeData(mac []byte, dataType int32, data []byte) error`

**Context for implementer:**

The existing server already sends events to Kafka (`eventStore`). The `EventBroker` is an additional in-process pub/sub bus for HTTP SSE clients — it does NOT replace Kafka. Add it alongside existing event publishing.

**`event_broker.go` — full implementation:**
```go
package mesh

import (
    "encoding/json"
    "sync"
    "time"
)

// EventType is a named SSE event category.
type EventType string

const (
    EventMotion      EventType = "motion"
    EventNodeOnline  EventType = "node_online"
    EventNodeOffline EventType = "node_offline"
    EventEnrolled    EventType = "enrolled"
    EventHealth      EventType = "health"
)

// Event is a single SSE message sent to subscribers.
type Event struct {
    Type      EventType       `json:"type"`
    Data      json.RawMessage `json:"data"`
    Timestamp time.Time       `json:"timestamp"`
}

// EventBroker is an in-process pub/sub bus. Publish is non-blocking; slow
// subscribers lose events silently rather than blocking the mesh message loop.
type EventBroker struct {
    mu      sync.RWMutex
    clients map[chan Event]struct{}
}

// NewEventBroker returns an initialised EventBroker.
func NewEventBroker() *EventBroker {
    return &EventBroker{clients: make(map[chan Event]struct{})}
}

// Subscribe returns a buffered channel that receives future events.
func (b *EventBroker) Subscribe() chan Event {
    ch := make(chan Event, 32)
    b.mu.Lock()
    b.clients[ch] = struct{}{}
    b.mu.Unlock()
    return ch
}

// Unsubscribe removes and closes a subscriber channel.
func (b *EventBroker) Unsubscribe(ch chan Event) {
    b.mu.Lock()
    delete(b.clients, ch)
    b.mu.Unlock()
    close(ch)
}

// Publish sends e to all subscribers. Drops the event for any subscriber
// whose buffer is full (non-blocking).
func (b *EventBroker) Publish(e Event) {
    b.mu.RLock()
    defer b.mu.RUnlock()
    for ch := range b.clients {
        select {
        case ch <- e:
        default:
        }
    }
}
```

**`node_registry.go` additions** (add after existing `GetAllNodes`):
```go
// GetNodeByID returns the node assigned the given logical ID, or false.
func (nr *NodeRegistry) GetNodeByID(nodeId uint8) (*NodeInfo, bool) {
    nr.mu.RLock()
    defer nr.mu.RUnlock()
    for _, n := range nr.nodes {
        if n.NodeID == nodeId {
            copy := *n
            return &copy, true
        }
    }
    return nil, false
}

// GetNodesByZone returns all nodes assigned to the named zone.
func (nr *NodeRegistry) GetNodesByZone(zone string) []*NodeInfo {
    nr.mu.RLock()
    defer nr.mu.RUnlock()
    var result []*NodeInfo
    for _, n := range nr.nodes {
        if n.Zone == zone {
            copy := *n
            result = append(result, &copy)
        }
    }
    return result
}
```

**`server.go` additions:**

Add to `MeshServer` struct (after `currentTxPreset uint8`):
```go
eventBroker     *EventBroker
nodeOnlineState map[string]bool // keyed by MACString; true = was online last check
```

Initialise in `NewMeshServer` (or wherever the struct is built):
```go
eventBroker:     NewEventBroker(),
nodeOnlineState: make(map[string]bool),
```

New methods on `MeshServer`:
```go
func (ms *MeshServer) GetEventBroker() *EventBroker { return ms.eventBroker }
func (ms *MeshServer) GetHealthTimeout() time.Duration { return ms.healthTimeout }

// SendNodeData sends a serial command frame to a specific node MAC.
func (ms *MeshServer) SendNodeData(mac []byte, dataType int32, data []byte) error {
    payload := make([]byte, MaxDataLength)
    copy(payload, data)
    msg := &MeshMessage{
        ProtoVersion: 2,
        MessageType:  MessageTypeSerialCmdBroadcast,
        DataType:     int32(dataType),
        Data:         payload,
    }
    copy(msg.TargetMacAddress, mac)
    return ms.serialComm.WriteFrame(msg)
}
```

**Publish helpers** (add to server.go):
```go
func (ms *MeshServer) publishEvent(eventType EventType, data interface{}) {
    raw, err := json.Marshal(data)
    if err != nil {
        return
    }
    ms.eventBroker.Publish(Event{Type: eventType, Data: raw, Timestamp: time.Now()})
}

// called from handlePIRData after existing Kafka publish:
func (ms *MeshServer) publishMotionEvent(node *NodeInfo) {
    ms.publishEvent(EventMotion, map[string]interface{}{
        "nodeId":    node.NodeID,
        "name":      node.Name,
        "zone":      node.Zone,
        "hopCount":  node.HopCount,
        "timestamp": time.Now().UTC().Format(time.RFC3339),
    })
}

// called from handleHealthData / handleSerialData after UpdateNode:
func (ms *MeshServer) publishHealthEvent(node *NodeInfo) {
    online := time.Since(node.LastSeen) <= ms.healthTimeout
    ms.publishEvent(EventHealth, map[string]interface{}{
        "nodeId":  node.NodeID,
        "name":    node.Name,
        "online":  online,
        "uptime":  node.Uptime,
        "hopCount": node.HopCount,
    })
    ms.mu.Lock()
    defer ms.mu.Unlock()
    if !ms.nodeOnlineState[node.MACString] {
        ms.nodeOnlineState[node.MACString] = true
        ms.publishEvent(EventNodeOnline, map[string]interface{}{
            "nodeId": node.NodeID,
            "name":   node.Name,
        })
    }
}

// called from ApproveEnrollment after JOIN_ACK:
func (ms *MeshServer) publishEnrolledEvent(node *NodeInfo, adapterTypeStr string) {
    ms.publishEvent(EventEnrolled, map[string]interface{}{
        "nodeId": node.NodeID,
        "name":   node.Name,
        "type":   adapterTypeStr,
    })
}
```

**Offline detector** (add to server.go):
```go
func (ms *MeshServer) offlineDetectorLoop() {
    ticker := time.NewTicker(30 * time.Second)
    defer ticker.Stop()
    for {
        select {
        case <-ticker.C:
            ms.checkOfflineNodes()
        case <-ms.ctx.Done():
            return
        }
    }
}

func (ms *MeshServer) checkOfflineNodes() {
    nodes := ms.nodeRegistry.GetAllNodes()
    ms.mu.Lock()
    defer ms.mu.Unlock()
    for _, node := range nodes {
        if time.Since(node.LastSeen) > ms.healthTimeout {
            if ms.nodeOnlineState[node.MACString] {
                ms.nodeOnlineState[node.MACString] = false
                ms.publishEvent(EventNodeOffline, map[string]interface{}{
                    "nodeId":   node.NodeID,
                    "name":     node.Name,
                    "lastSeen": node.LastSeen.UTC().Format(time.RFC3339),
                })
            }
        }
    }
}
```

Start the goroutine in `Start()` alongside existing goroutines:
```go
ms.wg.Add(1)
go func() {
    defer ms.wg.Done()
    ms.offlineDetectorLoop()
}()
```

Wire publish calls into existing handlers:
- In `handlePIRData` (after existing Kafka publish): `ms.publishMotionEvent(node)` where node is fetched from registry after UpdateNode
- In the `OpHealthReport`/`OpNodeHealth` case of `handleSerialData` (after UpdateNode): `ms.publishHealthEvent(node)`
- In `ApproveEnrollment` (after `AssignNode`): `ms.publishEnrolledEvent(node, "pir")` — use `GetAdapterTypeName` / type string from params if available (pass type string into publish)

For `publishEnrolledEvent`, `ApproveEnrollment` doesn't know the adapter type string. Add an `AdapterTypeStr` field to `ApprovalParams`:
```go
type ApprovalParams struct {
    NodeID          uint8
    Name            string
    Zone            string
    AdapterTypeStr  string // "pir", "led", etc. — for SSE enrolled event; empty = "unknown"
}
```
The v1 enrollment handler sets this; the existing `/api/enrollments/{mac}/approve` handler passes `ApprovalParams{}` (empty = "unknown"), which is backward-compatible.

- [ ] **Step 1: Write EventBroker tests**

  Create `server/orchestrator/mesh/event_broker_test.go`:
  ```go
  package mesh

  import (
      "testing"
      "time"
  )

  func TestEventBroker_SubscribeReceivesPublished(t *testing.T) {
      b := NewEventBroker()
      ch := b.Subscribe()
      defer b.Unsubscribe(ch)

      raw := []byte(`{"nodeId":7}`)
      b.Publish(Event{Type: EventMotion, Data: raw, Timestamp: time.Now()})

      select {
      case e := <-ch:
          if e.Type != EventMotion {
              t.Errorf("Type: got %q, want %q", e.Type, EventMotion)
          }
      case <-time.After(100 * time.Millisecond):
          t.Fatal("no event received within 100ms")
      }
  }

  func TestEventBroker_UnsubscribedChannelClosed(t *testing.T) {
      b := NewEventBroker()
      ch := b.Subscribe()
      b.Unsubscribe(ch)
      _, open := <-ch
      if open {
          t.Error("channel should be closed after Unsubscribe")
      }
  }

  func TestEventBroker_FullBufferDropsEvent(t *testing.T) {
      b := NewEventBroker()
      ch := b.Subscribe()
      defer b.Unsubscribe(ch)

      // Fill buffer (size 32) + 1 extra — must not block
      done := make(chan struct{})
      go func() {
          for i := 0; i < 33; i++ {
              b.Publish(Event{Type: EventHealth, Data: []byte(`{}`), Timestamp: time.Now()})
          }
          close(done)
      }()

      select {
      case <-done:
      case <-time.After(500 * time.Millisecond):
          t.Fatal("Publish blocked on full buffer")
      }

      if len(ch) != 32 {
          t.Errorf("buffer has %d events, want 32", len(ch))
      }
  }

  func TestEventBroker_MultipleSubscribers(t *testing.T) {
      b := NewEventBroker()
      ch1 := b.Subscribe()
      ch2 := b.Subscribe()
      defer b.Unsubscribe(ch1)
      defer b.Unsubscribe(ch2)

      b.Publish(Event{Type: EventNodeOnline, Data: []byte(`{}`), Timestamp: time.Now()})

      for i, ch := range []chan Event{ch1, ch2} {
          select {
          case e := <-ch:
              if e.Type != EventNodeOnline {
                  t.Errorf("subscriber %d: type %q, want %q", i, e.Type, EventNodeOnline)
              }
          case <-time.After(100 * time.Millisecond):
              t.Errorf("subscriber %d: no event received", i)
          }
      }
  }
  ```

- [ ] **Step 2: Run EventBroker tests to verify they fail**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestEventBroker -count=1 2>&1 | tail -5
  ```

  Expected: compile error (EventBroker not defined).

- [ ] **Step 3: Create `event_broker.go`**

  Write the full implementation shown in Context above to `server/orchestrator/mesh/event_broker.go`.

- [ ] **Step 4: Run EventBroker tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestEventBroker -count=1 2>&1 | tail -5
  ```

  Expected: `ok ... (all 4 pass)`.

- [ ] **Step 5: Write NodeRegistry helper tests**

  In `server/orchestrator/mesh/mesh_test.go`, add to `TestNodeRegistry`:
  ```go
  t.Run("GetNodeByID_ReturnsNode_WhenExists", func(t *testing.T) {
      registry := NewNodeRegistry()
      mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
      registry.AssignNode(mac, 7, "entrance-left", "lobby")
      node, ok := registry.GetNodeByID(7)
      if !ok { t.Fatal("expected node, got nothing") }
      if node.NodeID != 7 { t.Errorf("NodeID: %d, want 7", node.NodeID) }
      if node.Name != "entrance-left" { t.Errorf("Name: %q", node.Name) }
  })

  t.Run("GetNodeByID_ReturnsFalse_WhenMissing", func(t *testing.T) {
      registry := NewNodeRegistry()
      if _, ok := registry.GetNodeByID(99); ok {
          t.Error("expected false for unknown ID")
      }
  })

  t.Run("GetNodesByZone_ReturnsOnlyZoneNodes", func(t *testing.T) {
      registry := NewNodeRegistry()
      registry.AssignNode([]byte{0x01, 0, 0, 0, 0, 0}, 1, "a", "lobby")
      registry.AssignNode([]byte{0x02, 0, 0, 0, 0, 0}, 2, "b", "lobby")
      registry.AssignNode([]byte{0x03, 0, 0, 0, 0, 0}, 3, "c", "stage")
      nodes := registry.GetNodesByZone("lobby")
      if len(nodes) != 2 { t.Errorf("len: %d, want 2", len(nodes)) }
      for _, n := range nodes {
          if n.Zone != "lobby" { t.Errorf("unexpected zone %q", n.Zone) }
      }
  })

  t.Run("GetNodesByZone_ReturnsEmpty_WhenNoMatch", func(t *testing.T) {
      registry := NewNodeRegistry()
      registry.AssignNode([]byte{0x01, 0, 0, 0, 0, 0}, 1, "a", "lobby")
      nodes := registry.GetNodesByZone("nowhere")
      if len(nodes) != 0 { t.Errorf("len: %d, want 0", len(nodes)) }
  })
  ```

- [ ] **Step 6: Run to verify failure**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestNodeRegistry -count=1 2>&1 | tail -5
  ```

  Expected: compile error (methods not defined).

- [ ] **Step 7: Add `GetNodeByID` and `GetNodesByZone` to `node_registry.go`**

  Add both methods shown in Context above after the existing `GetAllNodes` method.

- [ ] **Step 8: Run NodeRegistry tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestNodeRegistry -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 9: Write MeshServer event-wiring tests**

  In `server/orchestrator/mesh/server_test.go`, add:
  ```go
  func TestMeshServer_PublishesMotionEvent_OnPIRData(t *testing.T) {
      ms := newTestMeshServer(t)
      ch := ms.GetEventBroker().Subscribe()
      defer ms.GetEventBroker().Unsubscribe(ch)

      mac := []byte{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x02}
      ms.nodeRegistry.AssignNode(mac, 5, "stage-left", "stage")

      data := make([]byte, MaxDataLength)
      data[0] = byte(AdapterTypePIR)
      copy(data[1:7], mac)
      msg := &MeshMessage{
          ProtoVersion:     2,
          MessageType:      MessageTypeAdapterData,
          DataType:         AdapterTypePIR,
          Data:             data,
          OriginMacAddress: mac,
      }
      if err := ms.handleMessage(msg); err != nil {
          t.Fatalf("handleMessage: %v", err)
      }

      select {
      case e := <-ch:
          if e.Type != EventMotion {
              t.Errorf("event type: %q, want %q", e.Type, EventMotion)
          }
      case <-time.After(200 * time.Millisecond):
          t.Fatal("no motion event within 200ms")
      }
  }

  func TestMeshServer_PublishesNodeOnline_OnFirstHealthReport(t *testing.T) {
      ms := newTestMeshServer(t)
      ch := ms.GetEventBroker().Subscribe()
      defer ms.GetEventBroker().Unsubscribe(ch)

      mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}
      data := make([]byte, MaxDataLength)
      data[0] = byte(OpHealthReport)
      data[1] = byte(AdapterTypePIR)
      copy(data[2:8], mac)
      msg := &MeshMessage{
          ProtoVersion:     2,
          MessageType:      MessageTypeAdapterData,
          DataType:         AdapterTypeSerial,
          Data:             data,
          OriginMacAddress: mac,
      }
      if err := ms.handleMessage(msg); err != nil {
          t.Fatalf("handleMessage: %v", err)
      }

      var gotOnline bool
      for {
          select {
          case e := <-ch:
              if e.Type == EventNodeOnline { gotOnline = true }
          case <-time.After(200 * time.Millisecond):
              if !gotOnline { t.Error("no node_online event") }
              return
          }
          if gotOnline { return }
      }
  }

  func TestMeshServer_CheckOfflineNodes_PublishesOfflineEvent(t *testing.T) {
      ms := newTestMeshServer(t)
      ch := ms.GetEventBroker().Subscribe()
      defer ms.GetEventBroker().Unsubscribe(ch)

      mac := []byte{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01}
      ms.nodeRegistry.AssignNode(mac, 3, "old-node", "lobby")
      macStr := macToString(mac)

      // mark as previously online
      ms.mu.Lock()
      ms.nodeOnlineState[macStr] = true
      // set LastSeen 80s ago — exceeds 75s threshold
      ms.nodeRegistry.nodes[macStr].LastSeen = time.Now().Add(-80 * time.Second)
      ms.mu.Unlock()

      ms.checkOfflineNodes()

      select {
      case e := <-ch:
          if e.Type != EventNodeOffline {
              t.Errorf("event type: %q, want %q", e.Type, EventNodeOffline)
          }
      case <-time.After(100 * time.Millisecond):
          t.Fatal("no node_offline event")
      }
  }
  ```

- [ ] **Step 10: Run to verify failure**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestMeshServer_Publish|TestMeshServer_Check" -count=1 2>&1 | tail -10
  ```

  Expected: compile errors (eventBroker field, methods not defined).

- [ ] **Step 11: Modify `server.go` with all EventBroker additions**

  Apply all changes described in Context above:
  1. Add fields to `MeshServer` struct
  2. Initialise in constructor
  3. Add `GetEventBroker`, `GetHealthTimeout`, `SendNodeData` methods
  4. Add `publishEvent`, `publishMotionEvent`, `publishHealthEvent`, `publishEnrolledEvent` helpers
  5. Add `offlineDetectorLoop` and `checkOfflineNodes`
  6. Start offline detector goroutine in `Start()`
  7. Add `AdapterTypeStr` field to `ApprovalParams`
  8. Wire publish calls into `handlePIRData`, health handler, `ApproveEnrollment`

  For step 8, find the exact locations:
  - `handlePIRData`: after `ms.eventStore.LogEvent(...)` or the equivalent Kafka publish, add:
    ```go
    if node, ok := ms.nodeRegistry.GetNode(mac); ok {
        ms.publishMotionEvent(node)
    }
    ```
  - Health handler (the `OpHealthReport`/`OpNodeHealth` case): after `ms.nodeRegistry.UpdateNode(...)`, add:
    ```go
    if node, ok := ms.nodeRegistry.GetNode(originMAC); ok {
        ms.publishHealthEvent(node)
    }
    ```
  - `ApproveEnrollment`: after `ms.nodeRegistry.AssignNode(...)`, add:
    ```go
    if node, ok := ms.nodeRegistry.GetNode(node.MAC[:]); ok {
        typeStr := params.AdapterTypeStr
        if typeStr == "" { typeStr = "unknown" }
        ms.publishEnrolledEvent(node, typeStr)
    }
    ```

- [ ] **Step 12: Run targeted tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestEventBroker|TestNodeRegistry|TestMeshServer_Publish|TestMeshServer_Check" -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 13: Run full test suite**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 2>&1 | tail -5
  ```

  Expected: `ok github.com/superbrobenji/motionServer/mesh`.

- [ ] **Step 14: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/event_broker.go \
    server/orchestrator/mesh/event_broker_test.go \
    server/orchestrator/mesh/node_registry.go \
    server/orchestrator/mesh/server.go \
    server/orchestrator/mesh/mesh_test.go \
    server/orchestrator/mesh/server_test.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit \
    -m "feat(server): EventBroker pub/sub, NodeRegistry helpers, MeshServer event wiring"
  ```

---

### Task 2: ZoneRegistry + /api/v1/zones

**Files:**
- Create: `server/orchestrator/mesh/zone_registry.go`
- Create: `server/orchestrator/mesh/api_v1_zones.go`
- Modify: `server/orchestrator/mesh/server.go` — add `zoneRegistry` field, `GetZoneRegistry()`, `SetZoneRegistryPath()`
- Modify: `server/orchestrator/mesh/api.go` — register `/api/v1/zones` routes

**Interfaces:**
- Consumes (from Task 1): `(*NodeRegistry).GetNodesByZone(zone string) []*NodeInfo`, `(*MeshServer).SendNodeData(mac []byte, dataType int32, data []byte) error`
- Produces: `NewZoneRegistry() *ZoneRegistry`
- Produces: `(*ZoneRegistry).Add(name string) (*ZoneV1, error)` — error if name already exists
- Produces: `(*ZoneRegistry).Get(id string) (*ZoneV1, bool)`
- Produces: `(*ZoneRegistry).List() []*ZoneV1`
- Produces: `(*ZoneRegistry).Update(id, newName string) (*ZoneV1, bool)`
- Produces: `(*ZoneRegistry).Delete(id string) bool`
- Produces: `(*ZoneRegistry).Persist(path string) error`
- Produces: `(*ZoneRegistry).Load(path string) error`
- Produces: `(*MeshServer).GetZoneRegistry() *ZoneRegistry`
- Produces: `(*MeshServer).SetZoneRegistryPath(path string)`
- Produces: `ZoneV1{ID string; Name string}` — shared response struct

**Context for implementer:**

Zone ID = `strings.ToLower(strings.ReplaceAll(name, " ", "-"))`. All spaces become hyphens, all uppercase becomes lowercase. Example: `"Main Hall"` → `"main-hall"`. ID is derived once at creation and never changes (the ID is used in URLs).

`ZoneRegistry` persists as a JSON array of `ZoneV1`. Use the same pattern as `NodeRegistry.Persist`/`Load`. Start the periodic persist loop from `MeshServer.Start()` only if `zoneRegistryPath != ""`.

Zone command: `POST /api/v1/zones/{id}/command {"action":"trigger","params":{}}`. Fan-out: find all nodes in zone via `GetNodesByZone(id)`, send command to each. Use `data[0] = 0xD0` as the trigger opcode placeholder. Return 404 if zone not found, 200 with `{"success":true,"data":{"sent":N}}` where N = number of nodes commanded.

**`zone_registry.go`:**
```go
package mesh

import (
    "encoding/json"
    "fmt"
    "os"
    "strings"
    "sync"
)

// ZoneV1 is the artist-facing zone representation.
type ZoneV1 struct {
    ID   string `json:"id"`
    Name string `json:"name"`
}

// ZoneRegistry stores artist-defined zones.
type ZoneRegistry struct {
    mu    sync.RWMutex
    zones map[string]*ZoneV1 // keyed by ID
}

// NewZoneRegistry returns an empty ZoneRegistry.
func NewZoneRegistry() *ZoneRegistry {
    return &ZoneRegistry{zones: make(map[string]*ZoneV1)}
}

// zoneSlug derives the URL-safe ID from a zone name.
func zoneSlug(name string) string {
    return strings.ReplaceAll(strings.ToLower(name), " ", "-")
}

// Add creates a zone. Returns an error if the derived ID is already taken.
func (zr *ZoneRegistry) Add(name string) (*ZoneV1, error) {
    id := zoneSlug(name)
    zr.mu.Lock()
    defer zr.mu.Unlock()
    if _, exists := zr.zones[id]; exists {
        return nil, fmt.Errorf("zone %q already exists", id)
    }
    z := &ZoneV1{ID: id, Name: name}
    zr.zones[id] = z
    copy := *z
    return &copy, nil
}

// Get returns a zone by ID.
func (zr *ZoneRegistry) Get(id string) (*ZoneV1, bool) {
    zr.mu.RLock()
    defer zr.mu.RUnlock()
    z, ok := zr.zones[id]
    if !ok { return nil, false }
    copy := *z
    return &copy, true
}

// List returns all zones (unordered).
func (zr *ZoneRegistry) List() []*ZoneV1 {
    zr.mu.RLock()
    defer zr.mu.RUnlock()
    result := make([]*ZoneV1, 0, len(zr.zones))
    for _, z := range zr.zones {
        copy := *z
        result = append(result, &copy)
    }
    return result
}

// Update renames a zone. The ID never changes.
func (zr *ZoneRegistry) Update(id, newName string) (*ZoneV1, bool) {
    zr.mu.Lock()
    defer zr.mu.Unlock()
    z, ok := zr.zones[id]
    if !ok { return nil, false }
    z.Name = newName
    copy := *z
    return &copy, true
}

// Delete removes a zone by ID. Returns false if not found.
func (zr *ZoneRegistry) Delete(id string) bool {
    zr.mu.Lock()
    defer zr.mu.Unlock()
    _, ok := zr.zones[id]
    delete(zr.zones, id)
    return ok
}

// Persist writes the registry to a JSON file.
func (zr *ZoneRegistry) Persist(path string) error {
    zr.mu.RLock()
    zones := make([]*ZoneV1, 0, len(zr.zones))
    for _, z := range zr.zones { zones = append(zones, z) }
    zr.mu.RUnlock()
    data, err := json.MarshalIndent(zones, "", "  ")
    if err != nil { return err }
    return os.WriteFile(path, data, 0644)
}

// Load reads zones from a JSON file. Non-existent file is a no-op.
func (zr *ZoneRegistry) Load(path string) error {
    data, err := os.ReadFile(path)
    if os.IsNotExist(err) { return nil }
    if err != nil { return err }
    var zones []*ZoneV1
    if err := json.Unmarshal(data, &zones); err != nil { return err }
    zr.mu.Lock()
    defer zr.mu.Unlock()
    for _, z := range zones {
        zr.zones[z.ID] = z
    }
    return nil
}
```

**server.go additions:**

Add to `MeshServer` struct:
```go
zoneRegistry     *ZoneRegistry
zoneRegistryPath string
```

Initialise in constructor:
```go
zoneRegistry: NewZoneRegistry(),
```

Add methods:
```go
func (ms *MeshServer) GetZoneRegistry() *ZoneRegistry { return ms.zoneRegistry }

func (ms *MeshServer) SetZoneRegistryPath(path string) {
    ms.zoneRegistryPath = path
    if path != "" {
        _ = ms.zoneRegistry.Load(path)
    }
}
```

In `Stop()`, add zone persistence (after node registry persist):
```go
if ms.zoneRegistryPath != "" {
    _ = ms.zoneRegistry.Persist(ms.zoneRegistryPath)
}
```

**`api_v1_zones.go`:**
```go
package mesh

import (
    "encoding/json"
    "net/http"

    "github.com/gorilla/mux"
)

func (api *APIServer) v1GetZones(w http.ResponseWriter, r *http.Request) {
    zones := api.meshServer.GetZoneRegistry().List()
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: zones})
}

func (api *APIServer) v1CreateZone(w http.ResponseWriter, r *http.Request) {
    var body struct{ Name string `json:"name"` }
    if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Name == "" {
        api.writeError(w, http.StatusBadRequest, "name is required")
        return
    }
    zone, err := api.meshServer.GetZoneRegistry().Add(body.Name)
    if err != nil {
        api.writeError(w, http.StatusConflict, err.Error())
        return
    }
    api.writeJSON(w, http.StatusCreated, APIResponse{Success: true, Data: zone})
}

func (api *APIServer) v1UpdateZone(w http.ResponseWriter, r *http.Request) {
    id := mux.Vars(r)["id"]
    var body struct{ Name string `json:"name"` }
    if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Name == "" {
        api.writeError(w, http.StatusBadRequest, "name is required")
        return
    }
    zone, ok := api.meshServer.GetZoneRegistry().Update(id, body.Name)
    if !ok {
        api.writeError(w, http.StatusNotFound, "zone not found")
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: zone})
}

func (api *APIServer) v1DeleteZone(w http.ResponseWriter, r *http.Request) {
    id := mux.Vars(r)["id"]
    if !api.meshServer.GetZoneRegistry().Delete(id) {
        api.writeError(w, http.StatusNotFound, "zone not found")
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Message: "zone deleted"})
}

func (api *APIServer) v1ZoneCommand(w http.ResponseWriter, r *http.Request) {
    id := mux.Vars(r)["id"]
    if _, ok := api.meshServer.GetZoneRegistry().Get(id); !ok {
        api.writeError(w, http.StatusNotFound, "zone not found")
        return
    }
    var body struct {
        Action string                 `json:"action"`
        Params map[string]interface{} `json:"params"`
    }
    if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Action == "" {
        api.writeError(w, http.StatusBadRequest, "action is required")
        return
    }
    if body.Action != "trigger" {
        api.writeError(w, http.StatusNotImplemented, "action not supported")
        return
    }
    nodes := api.meshServer.GetNodeRegistry().GetNodesByZone(id)
    payload := make([]byte, MaxDataLength)
    payload[0] = 0xD0 // trigger placeholder opcode
    sent := 0
    for _, node := range nodes {
        if err := api.meshServer.SendNodeData(node.MAC, int32(AdapterTypeSerial), payload); err == nil {
            sent++
        }
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: map[string]int{"sent": sent}})
}
```

**Route registration in `api.go`** `setupRoutes()` (add after existing route block):
```go
// /api/v1/zones
api.router.Handle("/api/v1/zones", middleware(http.HandlerFunc(api.v1GetZones))).Methods("GET")
api.router.Handle("/api/v1/zones", middleware(http.HandlerFunc(api.v1CreateZone))).Methods("POST")
api.router.Handle("/api/v1/zones/{id}", middleware(http.HandlerFunc(api.v1UpdateZone))).Methods("PATCH")
api.router.Handle("/api/v1/zones/{id}", middleware(http.HandlerFunc(api.v1DeleteZone))).Methods("DELETE")
api.router.Handle("/api/v1/zones/{id}/command", middleware(http.HandlerFunc(api.v1ZoneCommand))).Methods("POST")
```

Where `middleware` is the existing auth+instrument wrapper (match the pattern already used in `setupRoutes`).

- [ ] **Step 1: Write ZoneRegistry tests**

  In `server/orchestrator/mesh/mesh_test.go`, add:
  ```go
  func TestZoneRegistry_AddAndGet(t *testing.T) {
      zr := NewZoneRegistry()
      zone, err := zr.Add("Main Hall")
      if err != nil { t.Fatalf("Add: %v", err) }
      if zone.ID != "main-hall" { t.Errorf("ID: %q, want %q", zone.ID, "main-hall") }
      if zone.Name != "Main Hall" { t.Errorf("Name: %q", zone.Name) }
  }

  func TestZoneRegistry_Add_DuplicateReturnsError(t *testing.T) {
      zr := NewZoneRegistry()
      if _, err := zr.Add("lobby"); err != nil { t.Fatal(err) }
      if _, err := zr.Add("lobby"); err == nil { t.Error("expected error for duplicate") }
      // "Lobby" and "lobby" both map to "lobby" — also duplicate
      if _, err := zr.Add("Lobby"); err == nil { t.Error("expected error for case-variant duplicate") }
  }

  func TestZoneRegistry_List(t *testing.T) {
      zr := NewZoneRegistry()
      zr.Add("lobby")
      zr.Add("stage")
      zones := zr.List()
      if len(zones) != 2 { t.Errorf("len: %d, want 2", len(zones)) }
  }

  func TestZoneRegistry_Update(t *testing.T) {
      zr := NewZoneRegistry()
      zr.Add("lobby")
      z, ok := zr.Update("lobby", "Lobby Area")
      if !ok { t.Fatal("Update returned false") }
      if z.Name != "Lobby Area" { t.Errorf("Name: %q", z.Name) }
      if z.ID != "lobby" { t.Errorf("ID changed: %q", z.ID) }
  }

  func TestZoneRegistry_Delete(t *testing.T) {
      zr := NewZoneRegistry()
      zr.Add("lobby")
      if !zr.Delete("lobby") { t.Error("Delete returned false") }
      if _, ok := zr.Get("lobby"); ok { t.Error("zone still present after delete") }
      if zr.Delete("lobby") { t.Error("second delete should return false") }
  }

  func TestZoneRegistry_PersistAndLoad(t *testing.T) {
      path := t.TempDir() + "/zones.json"
      zr := NewZoneRegistry()
      zr.Add("lobby")
      zr.Add("stage")
      if err := zr.Persist(path); err != nil { t.Fatalf("Persist: %v", err) }
      zr2 := NewZoneRegistry()
      if err := zr2.Load(path); err != nil { t.Fatalf("Load: %v", err) }
      zones := zr2.List()
      if len(zones) != 2 { t.Errorf("len after load: %d, want 2", len(zones)) }
  }
  ```

- [ ] **Step 2: Run to verify failure**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestZoneRegistry -count=1 2>&1 | tail -5
  ```

  Expected: compile error.

- [ ] **Step 3: Create `zone_registry.go`**

  Write the full implementation above.

- [ ] **Step 4: Run ZoneRegistry tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestZoneRegistry -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 5: Write zone HTTP handler tests**

  Create `server/orchestrator/mesh/api_v1_zones_test.go`:
  ```go
  package mesh

  import (
      "bytes"
      "encoding/json"
      "net/http"
      "net/http/httptest"
      "testing"
  )

  func newV1TestServer(t *testing.T) (*APIServer, *MeshServer) {
      t.Helper()
      ms := newTestMeshServer(t)
      api := NewAPIServer(ms, "test-key", nil)
      return api, ms
  }

  func v1Request(t *testing.T, api *APIServer, method, path string, body interface{}) *httptest.ResponseRecorder {
      t.Helper()
      var bodyBytes []byte
      if body != nil {
          var err error
          bodyBytes, err = json.Marshal(body)
          if err != nil { t.Fatalf("marshal body: %v", err) }
      }
      req := httptest.NewRequest(method, path, bytes.NewReader(bodyBytes))
      req.Header.Set("Authorization", "Bearer test-key")
      if body != nil { req.Header.Set("Content-Type", "application/json") }
      w := httptest.NewRecorder()
      api.ServeHTTP(w, req)
      return w
  }

  func TestV1Zones_CreateAndList(t *testing.T) {
      api, _ := newV1TestServer(t)

      w := v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      if w.Code != http.StatusCreated { t.Fatalf("create: %d, want 201", w.Code) }

      w = v1Request(t, api, "GET", "/api/v1/zones", nil)
      if w.Code != http.StatusOK { t.Fatalf("list: %d", w.Code) }
      var resp APIResponse
      json.NewDecoder(w.Body).Decode(&resp)
      if !resp.Success { t.Error("expected success:true") }
  }

  func TestV1Zones_CreateDuplicate_Returns409(t *testing.T) {
      api, _ := newV1TestServer(t)
      v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      w := v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      if w.Code != http.StatusConflict { t.Errorf("got %d, want 409", w.Code) }
  }

  func TestV1Zones_Update(t *testing.T) {
      api, _ := newV1TestServer(t)
      v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      w := v1Request(t, api, "PATCH", "/api/v1/zones/lobby", map[string]string{"name": "Lobby Area"})
      if w.Code != http.StatusOK { t.Fatalf("update: %d", w.Code) }
  }

  func TestV1Zones_Delete(t *testing.T) {
      api, _ := newV1TestServer(t)
      v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      w := v1Request(t, api, "DELETE", "/api/v1/zones/lobby", nil)
      if w.Code != http.StatusOK { t.Fatalf("delete: %d", w.Code) }
      w = v1Request(t, api, "DELETE", "/api/v1/zones/lobby", nil)
      if w.Code != http.StatusNotFound { t.Errorf("second delete: %d, want 404", w.Code) }
  }

  func TestV1Zones_Command_UnknownZone_Returns404(t *testing.T) {
      api, _ := newV1TestServer(t)
      w := v1Request(t, api, "POST", "/api/v1/zones/nowhere/command",
          map[string]interface{}{"action": "trigger", "params": map[string]interface{}{}})
      if w.Code != http.StatusNotFound { t.Errorf("got %d, want 404", w.Code) }
  }

  func TestV1Zones_Command_UnsupportedAction_Returns501(t *testing.T) {
      api, _ := newV1TestServer(t)
      v1Request(t, api, "POST", "/api/v1/zones", map[string]string{"name": "lobby"})
      w := v1Request(t, api, "POST", "/api/v1/zones/lobby/command",
          map[string]interface{}{"action": "explode", "params": map[string]interface{}{}})
      if w.Code != http.StatusNotImplemented { t.Errorf("got %d, want 501", w.Code) }
  }
  ```

- [ ] **Step 6: Run to verify failure**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestV1Zones -count=1 2>&1 | tail -5
  ```

  Expected: compile errors (handlers not defined / routes not registered).

- [ ] **Step 7: Create `api_v1_zones.go`; add zone fields to `server.go`; register routes in `api.go`**

  Apply all three changes from Context above.

- [ ] **Step 8: Run targeted tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestZoneRegistry|TestV1Zones" -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 9: Run full suite**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 2>&1 | tail -5
  ```

  Expected: `ok`.

- [ ] **Step 10: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/zone_registry.go \
    server/orchestrator/mesh/api_v1_zones.go \
    server/orchestrator/mesh/api_v1_zones_test.go \
    server/orchestrator/mesh/server.go \
    server/orchestrator/mesh/api.go \
    server/orchestrator/mesh/mesh_test.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit \
    -m "feat(server): ZoneRegistry + /api/v1/zones CRUD and command endpoints"
  ```

---

### Task 3: /api/v1/nodes endpoints + shared v1 types

**Files:**
- Create: `server/orchestrator/mesh/api_v1_types.go` — `NodeV1`, type translation helpers
- Create: `server/orchestrator/mesh/api_v1_nodes.go` — all 5 node handlers
- Modify: `server/orchestrator/mesh/api.go` — register `/api/v1/nodes` routes

**Interfaces:**
- Consumes (from Task 1): `(*NodeRegistry).GetNodeByID(uint8) (*NodeInfo, bool)`, `(*MeshServer).SendNodeData(mac []byte, dataType int32, data []byte) error`, `(*MeshServer).GetHealthTimeout() time.Duration`
- Consumes (from Task 2): `ZoneV1` type (already in zone_registry.go)
- Produces: `NodeV1{ID uint8; Name string; Zone string; Type string; Online bool; HopCount uint32; Uptime uint32; LastSeen time.Time}` — the artist-facing node response struct
- Produces: `adapterTypeToString(t int32) string` — translates int to "pir"/"led"/"serial"/"unknown"
- Produces: `adapterTypeFromString(s string) (int32, bool)` — translates "pir"/"led" to int; false if unknown

**Context for implementer:**

**`api_v1_types.go`:**
```go
package mesh

import "time"

// NodeV1 is the artist-facing node representation.
type NodeV1 struct {
    ID       uint8     `json:"id"`
    Name     string    `json:"name"`
    Zone     string    `json:"zone"`
    Type     string    `json:"type"`
    Online   bool      `json:"online"`
    HopCount uint32    `json:"hopCount"`
    Uptime   uint32    `json:"uptime"`
    LastSeen time.Time `json:"lastSeen"`
}

// adapterTypeToString converts an internal adapter type to the artist-facing string.
func adapterTypeToString(t int32) string {
    switch t {
    case AdapterTypePIR:    return "pir"
    case AdapterTypeLED:    return "led"
    case AdapterTypeSerial: return "serial"
    default:                return "unknown"
    }
}

// adapterTypeFromString converts an artist-facing type string to an internal adapter type.
// Returns false if the string is not a writable type.
func adapterTypeFromString(s string) (int32, bool) {
    switch s {
    case "pir": return AdapterTypePIR, true
    case "led": return AdapterTypeLED, true
    default:    return 0, false
    }
}

// nodeToV1 converts an internal NodeInfo to the artist-facing NodeV1.
func nodeToV1(n *NodeInfo, timeout time.Duration) NodeV1 {
    return NodeV1{
        ID:       n.NodeID,
        Name:     n.Name,
        Zone:     n.Zone,
        Type:     adapterTypeToString(n.AdapterType),
        Online:   time.Since(n.LastSeen) <= timeout,
        HopCount: n.HopCount,
        Uptime:   n.Uptime,
        LastSeen: n.LastSeen,
    }
}
```

**`api_v1_nodes.go`:**
```go
package mesh

import (
    "encoding/json"
    "net/http"
    "strconv"

    "github.com/gorilla/mux"
)

func (api *APIServer) v1GetNodes(w http.ResponseWriter, r *http.Request) {
    timeout := api.meshServer.GetHealthTimeout()
    nodes := api.meshServer.GetNodeRegistry().GetAllNodes()
    result := make([]NodeV1, 0, len(nodes))
    for _, n := range nodes {
        if n.NodeID > 0 { // only include nodes with assigned IDs
            result = append(result, nodeToV1(n, timeout))
        }
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: result})
}

func (api *APIServer) v1GetNode(w http.ResponseWriter, r *http.Request) {
    id, err := parseNodeID(mux.Vars(r)["id"])
    if err != nil {
        api.writeError(w, http.StatusBadRequest, "invalid node id")
        return
    }
    node, ok := api.meshServer.GetNodeRegistry().GetNodeByID(id)
    if !ok {
        api.writeError(w, http.StatusNotFound, "node not found")
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: nodeToV1(node, api.meshServer.GetHealthTimeout())})
}

func (api *APIServer) v1UpdateNode(w http.ResponseWriter, r *http.Request) {
    id, err := parseNodeID(mux.Vars(r)["id"])
    if err != nil {
        api.writeError(w, http.StatusBadRequest, "invalid node id")
        return
    }
    node, ok := api.meshServer.GetNodeRegistry().GetNodeByID(id)
    if !ok {
        api.writeError(w, http.StatusNotFound, "node not found")
        return
    }

    var body struct {
        Name *string `json:"name"`
        Zone *string `json:"zone"`
        Type *string `json:"type"`
    }
    if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
        api.writeError(w, http.StatusBadRequest, "invalid request body")
        return
    }

    name := node.Name
    zone := node.Zone
    if body.Name != nil { name = *body.Name }
    if body.Zone != nil { zone = *body.Zone }

    api.meshServer.GetNodeRegistry().AssignNode(node.MAC, node.NodeID, name, zone)

    if body.Type != nil {
        adapterType, ok := adapterTypeFromString(*body.Type)
        if !ok {
            api.writeError(w, http.StatusBadRequest, "unknown type: "+*body.Type)
            return
        }
        if err := api.meshServer.ConfigureNode(node.MAC, adapterType); err != nil {
            api.writeError(w, http.StatusInternalServerError, "failed to configure node")
            return
        }
    }

    updated, _ := api.meshServer.GetNodeRegistry().GetNodeByID(id)
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: nodeToV1(updated, api.meshServer.GetHealthTimeout())})
}

func (api *APIServer) v1DeleteNode(w http.ResponseWriter, r *http.Request) {
    id, err := parseNodeID(mux.Vars(r)["id"])
    if err != nil {
        api.writeError(w, http.StatusBadRequest, "invalid node id")
        return
    }
    node, ok := api.meshServer.GetNodeRegistry().GetNodeByID(id)
    if !ok {
        api.writeError(w, http.StatusNotFound, "node not found")
        return
    }
    api.meshServer.GetNodeRegistry().RemoveNode(node.MAC)
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Message: "node removed"})
}

func (api *APIServer) v1NodeCommand(w http.ResponseWriter, r *http.Request) {
    id, err := parseNodeID(mux.Vars(r)["id"])
    if err != nil {
        api.writeError(w, http.StatusBadRequest, "invalid node id")
        return
    }
    node, ok := api.meshServer.GetNodeRegistry().GetNodeByID(id)
    if !ok {
        api.writeError(w, http.StatusNotFound, "node not found")
        return
    }
    var body struct {
        Action string                 `json:"action"`
        Params map[string]interface{} `json:"params"`
    }
    if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Action == "" {
        api.writeError(w, http.StatusBadRequest, "action is required")
        return
    }
    if body.Action != "trigger" {
        api.writeError(w, http.StatusNotImplemented, "action not supported")
        return
    }
    payload := make([]byte, MaxDataLength)
    payload[0] = 0xD0 // trigger placeholder opcode
    if err := api.meshServer.SendNodeData(node.MAC, int32(AdapterTypeSerial), payload); err != nil {
        api.writeError(w, http.StatusInternalServerError, "failed to send command")
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true})
}

// parseNodeID converts a URL path segment to a uint8 node ID (1-255).
func parseNodeID(s string) (uint8, error) {
    n, err := strconv.ParseUint(s, 10, 8)
    if err != nil || n == 0 { return 0, fmt.Errorf("invalid node id") }
    return uint8(n), nil
}
```

Note: add `"fmt"` to the imports in `api_v1_nodes.go`.

**Route registration in `api.go`** `setupRoutes()`:
```go
// /api/v1/nodes
api.router.Handle("/api/v1/nodes", middleware(http.HandlerFunc(api.v1GetNodes))).Methods("GET")
api.router.Handle("/api/v1/nodes/{id}", middleware(http.HandlerFunc(api.v1GetNode))).Methods("GET")
api.router.Handle("/api/v1/nodes/{id}", middleware(http.HandlerFunc(api.v1UpdateNode))).Methods("PATCH")
api.router.Handle("/api/v1/nodes/{id}", middleware(http.HandlerFunc(api.v1DeleteNode))).Methods("DELETE")
api.router.Handle("/api/v1/nodes/{id}/command", middleware(http.HandlerFunc(api.v1NodeCommand))).Methods("POST")
```

- [ ] **Step 1: Write node type translation tests**

  In `server/orchestrator/mesh/mesh_test.go`, add:
  ```go
  func TestAdapterTypeTranslation(t *testing.T) {
      cases := []struct{ t int32; s string }{
          {AdapterTypePIR, "pir"},
          {AdapterTypeLED, "led"},
          {AdapterTypeSerial, "serial"},
          {AdapterTypeUnknown, "unknown"},
          {999, "unknown"},
      }
      for _, c := range cases {
          if got := adapterTypeToString(c.t); got != c.s {
              t.Errorf("adapterTypeToString(%d) = %q, want %q", c.t, got, c.s)
          }
      }
      if v, ok := adapterTypeFromString("pir"); !ok || v != AdapterTypePIR {
          t.Errorf("adapterTypeFromString(pir): got %d,%v", v, ok)
      }
      if _, ok := adapterTypeFromString("serial"); ok {
          t.Error("serial should not be writable via type string")
      }
      if _, ok := adapterTypeFromString("unknown"); ok {
          t.Error("unknown should not be writable")
      }
  }
  ```

- [ ] **Step 2: Write node HTTP handler tests**

  Create `server/orchestrator/mesh/api_v1_nodes_test.go`:
  ```go
  package mesh

  import (
      "encoding/json"
      "net/http"
      "testing"
  )

  func setupNodeForV1Test(t *testing.T, ms *MeshServer) {
      t.Helper()
      mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
      ms.nodeRegistry.AssignNode(mac, 7, "entrance-left", "lobby")
      ms.nodeRegistry.UpdateNode(mac, AdapterTypePIR, 3600, 2)
  }

  func TestV1Nodes_GetAll_ReturnsAssignedNodes(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "GET", "/api/v1/nodes", nil)
      if w.Code != http.StatusOK { t.Fatalf("status: %d", w.Code) }
      var resp APIResponse
      json.NewDecoder(w.Body).Decode(&resp)
      if !resp.Success { t.Error("expected success") }
  }

  func TestV1Nodes_GetAll_ExcludesUnassignedNodes(t *testing.T) {
      api, ms := newV1TestServer(t)
      // Register node with UpdateNode only — no NodeID assigned
      mac := []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
      ms.nodeRegistry.UpdateNode(mac, AdapterTypePIR, 100, 1)
      w := v1Request(t, api, "GET", "/api/v1/nodes", nil)
      var resp APIResponse
      json.NewDecoder(w.Body).Decode(&resp)
      data, _ := json.Marshal(resp.Data)
      var nodes []NodeV1
      json.Unmarshal(data, &nodes)
      if len(nodes) != 0 { t.Errorf("got %d nodes, want 0 (unassigned excluded)", len(nodes)) }
  }

  func TestV1Nodes_GetByID_ReturnsNode(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "GET", "/api/v1/nodes/7", nil)
      if w.Code != http.StatusOK { t.Fatalf("status: %d", w.Code) }
  }

  func TestV1Nodes_GetByID_Returns404_WhenMissing(t *testing.T) {
      api, _ := newV1TestServer(t)
      w := v1Request(t, api, "GET", "/api/v1/nodes/99", nil)
      if w.Code != http.StatusNotFound { t.Errorf("got %d, want 404", w.Code) }
  }

  func TestV1Nodes_Update_ChangesNameAndZone(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "PATCH", "/api/v1/nodes/7",
          map[string]string{"name": "stage-right", "zone": "stage"})
      if w.Code != http.StatusOK { t.Fatalf("status: %d", w.Code) }
      node, _ := ms.nodeRegistry.GetNodeByID(7)
      if node.Name != "stage-right" { t.Errorf("Name: %q", node.Name) }
      if node.Zone != "stage" { t.Errorf("Zone: %q", node.Zone) }
  }

  func TestV1Nodes_Update_UnknownType_Returns400(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "PATCH", "/api/v1/nodes/7", map[string]string{"type": "toaster"})
      if w.Code != http.StatusBadRequest { t.Errorf("got %d, want 400", w.Code) }
  }

  func TestV1Nodes_Delete_RemovesNode(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "DELETE", "/api/v1/nodes/7", nil)
      if w.Code != http.StatusOK { t.Fatalf("delete: %d", w.Code) }
      if _, ok := ms.nodeRegistry.GetNodeByID(7); ok { t.Error("node still exists after delete") }
  }

  func TestV1Nodes_Command_UnsupportedAction_Returns501(t *testing.T) {
      api, ms := newV1TestServer(t)
      setupNodeForV1Test(t, ms)
      w := v1Request(t, api, "POST", "/api/v1/nodes/7/command",
          map[string]interface{}{"action": "explode", "params": map[string]interface{}{}})
      if w.Code != http.StatusNotImplemented { t.Errorf("got %d, want 501", w.Code) }
  }
  ```

- [ ] **Step 3: Run to verify failure**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestAdapterType|TestV1Nodes" -count=1 2>&1 | tail -5
  ```

  Expected: compile errors.

- [ ] **Step 4: Create `api_v1_types.go` and `api_v1_nodes.go`; register routes in `api.go`**

  Write both files from Context above. Register the 5 node routes in `setupRoutes()`.

- [ ] **Step 5: Run targeted tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestAdapterType|TestV1Nodes" -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 6: Run full suite**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 2>&1 | tail -5
  ```

  Expected: `ok`.

- [ ] **Step 7: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/api_v1_types.go \
    server/orchestrator/mesh/api_v1_nodes.go \
    server/orchestrator/mesh/api_v1_nodes_test.go \
    server/orchestrator/mesh/api.go \
    server/orchestrator/mesh/mesh_test.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit \
    -m "feat(server): /api/v1/nodes CRUD + command endpoints, type translation helpers"
  ```

---

### Task 4: SSE endpoint + /api/v1/events + status + enrollments

**Files:**
- Create: `server/orchestrator/mesh/api_v1_events.go` — SSE handler
- Create: `server/orchestrator/mesh/api_v1_status.go` — `/api/v1/status` handler
- Create: `server/orchestrator/mesh/api_v1_enrollments.go` — `/api/v1/enrollments/*` handlers
- Modify: `server/orchestrator/mesh/api.go` — register all remaining `/api/v1/` routes

**Interfaces:**
- Consumes (from Task 1): `(*MeshServer).GetEventBroker() *EventBroker`, `(*EventBroker).Subscribe() chan Event`, `(*EventBroker).Unsubscribe(ch chan Event)`
- Consumes (from Task 3): `adapterTypeFromString(s string) (int32, bool)` — used in v1 approve to map type → adapterType

**Context for implementer:**

**SSE format** (exact, per spec):
```
event: motion
data: {"nodeId":7,"name":"entrance-left","zone":"lobby","hopCount":2,"timestamp":"2026-06-29T14:32:00Z"}

```
(blank line between events is part of the `\n\n` in the format string)

**`api_v1_events.go`:**
```go
package mesh

import (
    "encoding/json"
    "fmt"
    "net/http"
)

func (api *APIServer) v1Events(w http.ResponseWriter, r *http.Request) {
    flusher, ok := w.(http.Flusher)
    if !ok {
        http.Error(w, "streaming not supported", http.StatusInternalServerError)
        return
    }
    w.Header().Set("Content-Type", "text/event-stream")
    w.Header().Set("Cache-Control", "no-cache")
    w.Header().Set("Connection", "keep-alive")
    w.Header().Set("X-Accel-Buffering", "no") // disable nginx buffering if behind proxy

    ch := api.meshServer.GetEventBroker().Subscribe()
    defer api.meshServer.GetEventBroker().Unsubscribe(ch)

    for {
        select {
        case event, open := <-ch:
            if !open { return }
            data, err := json.Marshal(event.Data)
            if err != nil { continue }
            fmt.Fprintf(w, "event: %s\ndata: %s\n\n", event.Type, data)
            flusher.Flush()
        case <-r.Context().Done():
            return
        }
    }
}
```

**`api_v1_status.go`:**
```go
package mesh

import "net/http"

func (api *APIServer) v1Status(w http.ResponseWriter, r *http.Request) {
    timeout := api.meshServer.GetHealthTimeout()
    allNodes := api.meshServer.GetNodeRegistry().GetAllNodes()
    total := 0
    online := 0
    for _, n := range allNodes {
        if n.NodeID > 0 {
            total++
            // import "time" for time.Since
            if isOnline(n, timeout) { online++ }
        }
    }
    api.writeJSON(w, http.StatusOK, APIResponse{
        Success: true,
        Data: map[string]interface{}{
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

Add helper at bottom of `api_v1_status.go`:
```go
import "time"

func isOnline(n *NodeInfo, timeout time.Duration) bool {
    return time.Since(n.LastSeen) <= timeout
}
```

**`api_v1_enrollments.go`:**
```go
package mesh

import (
    "encoding/json"
    "net/http"

    "github.com/gorilla/mux"
)

func (api *APIServer) v1GetPendingEnrollments(w http.ResponseWriter, r *http.Request) {
    pending := api.meshServer.GetPendingEnrollments()
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: pending})
}

func (api *APIServer) v1GetAllEnrollments(w http.ResponseWriter, r *http.Request) {
    all := api.meshServer.GetAuthRegistry().GetAll()
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Data: all})
}

func (api *APIServer) v1ApproveEnrollment(w http.ResponseWriter, r *http.Request) {
    mac := mux.Vars(r)["mac"]
    var body struct {
        Name   string `json:"name"`
        Zone   string `json:"zone"`
        Type   string `json:"type"`
        NodeID uint8  `json:"nodeId"`
    }
    if r.Body != nil {
        _ = json.NewDecoder(r.Body).Decode(&body) // body optional
    }
    params := ApprovalParams{
        NodeID:         body.NodeID,
        Name:           body.Name,
        Zone:           body.Zone,
        AdapterTypeStr: body.Type,
    }
    if err := api.meshServer.ApproveEnrollment(mac, params); err != nil {
        api.writeError(w, http.StatusBadRequest, err.Error())
        return
    }
    if body.Type != "" {
        if adapterType, ok := adapterTypeFromString(body.Type); ok {
            // look up node by MAC to get the []byte form
            // ApproveEnrollment has already assigned the node; find it
            registry := api.meshServer.GetAuthRegistry()
            if node, err := registry.Get(mac); err == nil {
                _ = api.meshServer.ConfigureNode(node.MAC[:], adapterType)
            }
        }
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Message: "enrollment approved"})
}

func (api *APIServer) v1RejectEnrollment(w http.ResponseWriter, r *http.Request) {
    mac := mux.Vars(r)["mac"]
    if err := api.meshServer.RejectEnrollment(mac); err != nil {
        api.writeError(w, http.StatusBadRequest, err.Error())
        return
    }
    api.writeJSON(w, http.StatusOK, APIResponse{Success: true, Message: "enrollment rejected"})
}
```

Note: `api.meshServer.GetAuthRegistry().Get(mac)` — check if `nodeauth.Registry` has a `Get(mac string) (*nodeauth.NodeAuth, error)` method. If not, use `GetAll()` and filter by MAC string. Use whichever pattern already exists in the auth registry.

**Route registration in `api.go`** `setupRoutes()`:
```go
// /api/v1/events
api.router.Handle("/api/v1/events", middleware(http.HandlerFunc(api.v1Events))).Methods("GET")

// /api/v1/status
api.router.Handle("/api/v1/status", middleware(http.HandlerFunc(api.v1Status))).Methods("GET")

// /api/v1/enrollments
api.router.Handle("/api/v1/enrollments/pending", middleware(http.HandlerFunc(api.v1GetPendingEnrollments))).Methods("GET")
api.router.Handle("/api/v1/enrollments", middleware(http.HandlerFunc(api.v1GetAllEnrollments))).Methods("GET")
api.router.Handle("/api/v1/enrollments/{mac}/approve", middleware(http.HandlerFunc(api.v1ApproveEnrollment))).Methods("POST")
api.router.Handle("/api/v1/enrollments/{mac}/reject", middleware(http.HandlerFunc(api.v1RejectEnrollment))).Methods("POST")
```

**Testing SSE:** Use a `testFlusher` wrapper to capture streamed output:
```go
// testFlusher wraps httptest.ResponseRecorder and implements http.Flusher.
type testFlusher struct {
    *httptest.ResponseRecorder
}
func (tf *testFlusher) Flush() {}
```

For the SSE test:
1. Create a `testFlusher`
2. Run `api.v1Events(tf, req)` in a goroutine (it blocks until context cancels)
3. Publish an event via `broker.Publish(...)`
4. Cancel the request context to exit the handler
5. Assert the body contains `"event: motion"` and `"data: "`

```go
func TestV1Events_StreamsEventToClient(t *testing.T) {
    api, ms := newV1TestServer(t)
    ctx, cancel := context.WithCancel(context.Background())
    defer cancel()

    req := httptest.NewRequest("GET", "/api/v1/events", nil).WithContext(ctx)
    req.Header.Set("Authorization", "Bearer test-key")
    tf := &testFlusher{httptest.NewRecorder()}

    done := make(chan struct{})
    go func() {
        defer close(done)
        api.v1Events(tf, req)
    }()

    // Give handler time to subscribe
    time.Sleep(10 * time.Millisecond)

    raw, _ := json.Marshal(map[string]interface{}{"nodeId": 7})
    ms.GetEventBroker().Publish(Event{Type: EventMotion, Data: raw, Timestamp: time.Now()})

    // Give handler time to write
    time.Sleep(50 * time.Millisecond)
    cancel()
    <-done

    body := tf.Body.String()
    if !strings.Contains(body, "event: motion") {
        t.Errorf("body missing event line; got: %q", body)
    }
    if !strings.Contains(body, "data: ") {
        t.Errorf("body missing data line; got: %q", body)
    }
}
```

Import `"context"`, `"strings"`, `"time"`, `"encoding/json"`, `"net/http/httptest"` at top of the test file.

- [ ] **Step 1: Write SSE, status, and enrollment tests**

  Create `server/orchestrator/mesh/api_v1_events_test.go`:
  ```go
  package mesh

  import (
      "context"
      "encoding/json"
      "net/http"
      "net/http/httptest"
      "strings"
      "testing"
      "time"
  )

  type testFlusher struct {
      *httptest.ResponseRecorder
  }
  func (tf *testFlusher) Flush() {}

  func TestV1Events_StreamsEventToClient(t *testing.T) {
      api, ms := newV1TestServer(t)
      ctx, cancel := context.WithCancel(context.Background())
      defer cancel()

      req := httptest.NewRequest("GET", "/api/v1/events", nil).WithContext(ctx)
      req.Header.Set("Authorization", "Bearer test-key")
      tf := &testFlusher{httptest.NewRecorder()}

      done := make(chan struct{})
      go func() {
          defer close(done)
          api.v1Events(tf, req)
      }()

      time.Sleep(10 * time.Millisecond)
      raw, _ := json.Marshal(map[string]interface{}{"nodeId": 7})
      ms.GetEventBroker().Publish(Event{Type: EventMotion, Data: raw, Timestamp: time.Now()})
      time.Sleep(50 * time.Millisecond)
      cancel()
      <-done

      body := tf.Body.String()
      if !strings.Contains(body, "event: motion") {
          t.Errorf("body missing event line; got: %q", body)
      }
      if !strings.Contains(body, "data: ") {
          t.Errorf("body missing data line; got: %q", body)
      }
  }

  func TestV1Events_NonFlusher_Returns500(t *testing.T) {
      api, _ := newV1TestServer(t)
      req := httptest.NewRequest("GET", "/api/v1/events", nil)
      req.Header.Set("Authorization", "Bearer test-key")
      w := httptest.NewRecorder() // does not implement http.Flusher
      api.v1Events(w, req)
      if w.Code != http.StatusInternalServerError {
          t.Errorf("got %d, want 500", w.Code)
      }
  }

  func TestV1Status_ReturnsStructuredStatus(t *testing.T) {
      api, ms := newV1TestServer(t)
      mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
      ms.nodeRegistry.AssignNode(mac, 1, "test-node", "lobby")
      ms.nodeRegistry.UpdateNode(mac, AdapterTypePIR, 100, 1)

      w := v1Request(t, api, "GET", "/api/v1/status", nil)
      if w.Code != http.StatusOK { t.Fatalf("status: %d", w.Code) }
      var resp APIResponse
      json.NewDecoder(w.Body).Decode(&resp)
      if !resp.Success { t.Error("expected success") }
  }

  func TestV1Enrollments_GetPending_ReturnsOK(t *testing.T) {
      api, _ := newV1TestServer(t)
      w := v1Request(t, api, "GET", "/api/v1/enrollments/pending", nil)
      if w.Code != http.StatusOK { t.Fatalf("got %d, want 200", w.Code) }
  }

  func TestV1Enrollments_GetAll_ReturnsOK(t *testing.T) {
      api, _ := newV1TestServer(t)
      w := v1Request(t, api, "GET", "/api/v1/enrollments", nil)
      if w.Code != http.StatusOK { t.Fatalf("got %d, want 200", w.Code) }
  }
  ```

- [ ] **Step 2: Run to verify failure**

  ```bash
  cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestV1Events|TestV1Status|TestV1Enrollments" -count=1 2>&1 | tail -5
  ```

  Expected: compile errors.

- [ ] **Step 3: Create `api_v1_events.go`, `api_v1_status.go`, `api_v1_enrollments.go`; register routes in `api.go`**

  Write all three files from Context above. Check `nodeauth.Registry` for the `Get(mac)` method before writing `v1ApproveEnrollment` — if it doesn't exist, filter `GetAll()` in the handler instead. Add all remaining `/api/v1/` routes to `setupRoutes()`.

- [ ] **Step 4: Run targeted tests to verify they pass**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run "TestV1Events|TestV1Status|TestV1Enrollments" -count=1 2>&1 | tail -5
  ```

  Expected: all pass.

- [ ] **Step 5: Run full test suite**

  ```bash
  GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -count=1 2>&1 | tail -5
  ```

  Expected: `ok github.com/superbrobenji/motionServer/mesh`.

- [ ] **Step 6: Commit**

  ```bash
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer add \
    server/orchestrator/mesh/api_v1_events.go \
    server/orchestrator/mesh/api_v1_events_test.go \
    server/orchestrator/mesh/api_v1_status.go \
    server/orchestrator/mesh/api_v1_enrollments.go \
    server/orchestrator/mesh/api.go
  git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer commit \
    -m "feat(server): /api/v1/events SSE, /api/v1/status, /api/v1/enrollments"
  ```
