# Planetopia Architecture Review & Change Spec

**Date:** 2026-06-29
**Scope:** Full-stack review — firmware (Planetopia-nodes) + server (motionSensorServer)
**Context:** Interactive art installations, 20–50 ESP32 nodes, multi-hop ESP-NOW mesh, bidirectional sensor-to-effect reactions, 50–200ms latency budget, USB-serial gateway to Go orchestrator, artist-facing public API
**Breaking changes:** Freely permitted — no current deployments.

---

## 1. Problem Statement

Planetopia is a platform for interactive art installations. Artists deploy 20–50 ESP32 nodes in a venue. PIR sensors detect motion; the server reacts by sending commands to other nodes (LEDs, sound triggers, custom outputs). Artists write reaction logic against a clean public JSON API — the platform handles all mesh protocol details transparently.

The current architecture has a correct security model, solid serialization (nanopb/protobuf), and good layering. However, two critical routing bugs mean the mesh silently fails for any node more than one hop from the master, making multi-room installations non-functional. The server exposes low-level protocol details to callers rather than a clean artist-facing abstraction.

---

## 2. Current Architecture (as-built)

```
[Artist Code]
     ↕ REST API (polling, low-level, MAC-addressed)
[orchestrator — Go]
     ↕ Kafka (event log)
     ↕ USB serial / nanopb protobuf frames
[ESP32 master node — Serial_Adapter]
     ↕ ESP-NOW (AES encrypted, ECDH per-peer LMK post-enrollment)
[ESP32 sensor nodes — PIR_Adapter, etc.]
```

### What works well
- ESP-NOW with ECDH enrollment + per-peer LMK: solid security model for embedded hardware
- Replay protection (boot epoch + seq ring buffer): covers reboot attacks
- TOFU master MAC enforcement: anti-impersonation
- Deferred EEPROM writes (dirty-flag pattern): flash wear protection
- Relay jitter on beacons (10–73ms random): eliminates collision bursts
- nanopb over length-prefixed serial frames: compact, correct framing
- Docker-composed server with Kafka for event sourcing: good foundation for artist API
- Adapter factory pattern: clean hardware abstraction

---

## 3. Critical Bugs

### 3.1 Multi-hop sensor data relay is missing (firmware)

**File:** `main/src/Mesh/Mesh.cpp:924` — `processAdapterData()`

When an intermediate node receives `MESH_TYPE_ADAPTER_DATA` not targeted at itself, it calls `externalRecvCallback(msg)` → `adapter->onMeshData(msg)` → `PIR_Adapter::onMeshDataImpl()` → **no-op**. Message dropped.

The routing table is correct (beacon propagation builds `currentMaster.nextHop` for every node), but the relay mechanism for sensor data does not exist. Every node beyond direct ESP-NOW range of the master is silently offline.

**Impact:** Multi-room installations non-functional without this fix.

### 3.2 Multi-hop enrollment (JOIN_ACK) cannot propagate (firmware)

**File:** `main/src/Mesh/Mesh.cpp:1040` — `processJoinAck()`

`processJoinAck()` only processes the ACK if addressed to the current node. No relay for nodes beyond one hop. Nodes more than one hop from master can never enroll.

### 3.3 TX power set sends raw bytes, bypasses protobuf (server)

**File:** `server/orchestrator/mesh/server.go:584` — `SetTxPowerPreset()`

Builds raw `[0xA1, preset]` with 2-byte length header via `WriteRaw()`. Firmware's `handleCompleteFrame()` tries to nanopb-decode every frame — this fails silently. TX power set from server does nothing.

---

## 4. High-Priority Missing Features

### 4.1 PIR nodes invisible to server

Health reports (`0xB1`) sent only by `Serial_Adapter` (master). PIR nodes never appear in `NodeRegistry`. Server has no visibility into whether sensor nodes are alive.

### 4.2 No real-time event push to artists

Dashboard and external code poll REST endpoints. No SSE or WebSocket endpoint. Art reaction loop cannot meet 50–200ms latency budget via polling.

### 4.3 No targeted node command API

`BuildAdapterDataMessage()` exists but no HTTP endpoint. Artists cannot send custom payloads to a specific node.

### 4.4 No reaction loop infrastructure

Kafka has `motion-trigger` topic but nothing consumes it. Path from "PIR event received" to "command sent to another node" is unimplemented.

### 4.5 No artist abstraction layer

Existing API exposes MAC addresses, adapter type integers, raw opcodes, and protobuf field semantics. Artists must understand the mesh protocol to use it.

---

## 5. Proposed Changes

### 5.1 Multi-hop uplink relay (firmware — critical)

Add relay to `Mesh::processAdapterData()`:

```cpp
void Mesh::processAdapterData(const mesh_message& msg) {
  // Relay toward master if we're not the destination
  if (!isMaster &&
      memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0 &&
      msg.hopCount < planetopia::config::MAX_HOPS) {
    if (isReplay(msg)) return;
    mesh_message relay = msg;
    relay.hopCount++;
    memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
    transmitCore(relay.dataType, relay.data, MESH_TYPE_ADAPTER_DATA, &relay);
    return;
  }
  // existing config opcode guard + externalRecvCallback
}
```

Existing replay cache (16-entry ring, `mac+epoch+seq`) prevents loops.

### 5.2 Multi-hop downlink relay (firmware — critical)

Add `relayDownlink()` helper:

```cpp
void Mesh::relayDownlink(const mesh_message& msg) {
  if (isReplay(msg)) return;
  if (msg.hopCount >= planetopia::config::MAX_HOPS) return;
  mesh_message relay = msg;
  relay.hopCount++;
  memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
  broadcastToAllPeers(relay);
}
```

Called from `processAdapterData()` for:
- `MESH_TYPE_SERIAL_CMD_BROADCAST` — always relay outward
- `MESH_TYPE_ADAPTER_DATA` where target is not self and not master

### 5.3 Multi-hop JOIN_ACK relay (firmware — critical)

In `processJoinAck()`, before the target check:

```cpp
if (memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0 &&
    !isReplay(msg) &&
    msg.hopCount < planetopia::config::MAX_HOPS) {
  relayDownlink(msg);
  return;
}
```

### 5.4 Increase data field to 64 bytes (firmware + server — breaking)

`mesh_message.data[12]` → `data[64]`. `mesh_message` grows from 75B to 127B — still well within ESP-NOW's 250B limit.

**Firmware:**
- Update `struct mesh_message` field declaration
- Update `static_assert(sizeof(mesh_message) == 75)` → `static_assert(sizeof(mesh_message) == 127)`
- Update `mesh.proto` data field description
- Update nanopb `.options` max size for data field
- Regenerate `mesh.pb.h` / `mesh.pb.c`
- Bump `PROTO_VERSION = 2`

**Server:**
- `MaxDataLength = 12` → `MaxDataLength = 64` in `constants.go`
- Update `ParseHealthReport()` bounds check (still uses first 12 bytes, unchanged)
- Add proto version guard in `handleMessage()`: drop messages with `ProtoVersion > 0 && ProtoVersion != 2`

### 5.5 Remove 3-chunk enrollment workaround (firmware — breaking)

`sendEnrollmentRequest()` currently splits the 32-byte public key across 3 messages of 11 bytes each because `data[12]` couldn't hold it. The key already lives in `enrollmentPublicKey[32]` — the chunking was purely a workaround for the data field size.

With `data[64]`, remove `sendEnrollmentRequest()`'s chunked send. Send a single `MESH_TYPE_ENROLLMENT` message with `enrollmentPublicKey` populated. Remove chunk accumulation state in `processEnrollmentRequest()`. Simplifies both send and receive paths significantly.

### 5.6 PIR health heartbeat (firmware + server)

Non-serial nodes need to report health through the mesh.

**New opcode:** `OP_NODE_HEALTH = 0xB2`

**Firmware:** Every non-master node sends a health packet every 30s via `MESH_TYPE_ADAPTER_DATA` with `dataType=SERIAL_ADAPTER`:

```
data[0]    = 0xB2           (OP_NODE_HEALTH)
data[1]    = adapterType    (int8)
data[2..7] = own MAC        (6 bytes)
data[8..11]= uptime seconds (uint32 LE)
```

`hopCount` in the message header carries hop distance.

Add to `PIR_Adapter::loop()` (and any future adapter): periodic call to `sendDataThroughMesh(SERIAL_ADAPTER, healthPayload)`.

**Server:** Add `OpNodeHealth byte = 0xB2` to `constants.go`. In `handleSerialData()`, add case for `0xB2` calling `UpdateNode()` with parsed MAC, adapter type, uptime, and `msg.HopCount`.

### 5.7 Fix TX power protobuf wrapping (server — bug fix)

Replace `WriteRaw()` in `SetTxPowerPreset()`:

```go
func (ms *MeshServer) SetTxPowerPreset(preset uint8) error {
    payload := make([]byte, MaxDataLength)
    payload[0] = OpTxPowerSet
    payload[1] = preset
    msg := &MeshMessage{
        MessageType: MessageTypeAdapterData,
        DataType:    AdapterTypeSerial,
        Data:        payload,
    }
    return ms.serialComm.WriteFrame(msg)
}
```

### 5.8 Node identity: logical ID + 7-seg display (firmware + server)

#### Logical Node ID

Server assigns a `uint8_t` node ID (1–255) at enrollment approval. Stored in EEPROM. Displayed on 7-seg. Stable across MAC changes (hotswap).

New EEPROM field: `NODE_ID` at address 496 (1 byte). Total used: 497 bytes — fits in 512.

New opcode `OP_NODE_ID_SET = 0xC0`:
```
data[0] = 0xC0
data[1] = nodeId (uint8_t)
```
Sent by server after JOIN_ACK approval. Node saves to EEPROM and updates display.

#### 7-seg display states

| State | Display | Implementation |
|---|---|---|
| Boot self-test | `8888` 500ms | Existing |
| Unenrolled | Flashing `----` | `setSegments({0x40,0x40,0x40,0x40})`, toggle 500ms in loop |
| Enrolled, no ID yet | `   0` | `show(0, false)` |
| Enrolled, sensor node | Node ID right-aligned: `  07` | `show(nodeId, false)` |
| Master node | Node ID + decimal point on digit 3 | `show(nodeId, false)` + DP bit on last segment |
| Error | Error code (overrides above) | Existing ErrorCore |

The decimal point distinguishes master from sensor node without new font characters. `setSegments()` already supports setting the DP bit (bit 7) directly.

#### Server-side node ID management

`NodeInfo` gains `NodeID uint8_t` and `Name string` and `Zone string` fields, persisted in node registry JSON.

`POST /api/v1/enrollments/{mac}/approve` body:
```json
{
  "name": "entrance-left",
  "zone": "lobby",
  "type": "pir",
  "nodeId": 7
}
```
If `nodeId` omitted, server auto-assigns next free ID.

On approval, server sends JOIN_ACK then immediately sends `OP_NODE_ID_SET` and `OP_CONFIG_SET` (adapter type).

### 5.9 Node hotswap (firmware + server)

Hotswap = replace a failed node with a new ESP32 that takes over its logical ID, adapter config, and position in the mesh.

**Workflow:**
1. Node fails → server marks it offline after 75s missed heartbeats
2. New node boots, broadcasts enrollment request
3. Dashboard shows new pending enrollment alongside existing offline node
4. Admin approves with `nodeId` matching the offline node's ID
5. Server sends JOIN_ACK + `OP_NODE_ID_SET` (same ID) + `OP_CONFIG_SET` (same adapter type from old node's last known config)
6. New node enrolls, shows same ID on 7-seg, resumes mesh participation
7. Server updates node registry: new MAC mapped to existing node ID, `online` flips to true

**Server responsibility:** When approving with an existing `nodeId`, auto-apply the previous node's `type` and `zone` unless explicitly overridden in the approval body. Old MAC entry is superseded (kept in registry as `{"status": "replaced", "replacedBy": newMac}`).

### 5.10 Dual master — backup master (firmware + server — Option B)

**Firmware:**

Both master nodes run `isMaster=true` from EEPROM. Both broadcast beacons. Both have `Serial_Adapter`.

Current "multiple masters detected" warning in `processMasterBeacon()` fires when two master MACs are seen. Add `DUAL_MASTER_MODE` flag to `project_config.h`. When true, suppress the multiple-masters error and accept beacons from both known master MACs (stored as `KNOWN_MASTER_MAC_PRIMARY` and `KNOWN_MASTER_MAC_SECONDARY` in EEPROM).

Sensor nodes learn routes to both masters via beacon propagation. They route toward whichever master has lower hop count (shortest path).

**Server:**

`MeshServerConfig` gains `SerialPortSecondary string`. When set, server starts a second `MeshServer` instance on the secondary port.

Primary/standby logic:
- Both instances feed into the same message pipeline and node registry
- "Active" is whichever received a frame more recently
- If primary has no frames for >75s, secondary is promoted to active for outgoing commands
- Both always receive (no messages dropped on standby)
- Artist API is unaware of which master is active — transparent failover

New env vars: `SERIAL_PORT_SECONDARY`, `DUAL_MASTER_ENABLED=true`.

Dashboard shows: `Primary master: online`, `Backup master: online/offline`.

### 5.11 Artist API layer — `/api/v1/` (server)

Clean JSON abstraction over the mesh protocol. Artists never see MAC addresses, adapter type integers, opcodes, or protobuf semantics.

#### Nodes

```
GET    /api/v1/nodes
GET    /api/v1/nodes/{id}
PATCH  /api/v1/nodes/{id}          {"name":"...", "zone":"...", "type":"pir"}
POST   /api/v1/nodes/{id}/command  {"action":"trigger", "params":{...}}
DELETE /api/v1/nodes/{id}
```

`GET /api/v1/nodes` response:
```json
[
  {
    "id": 7,
    "name": "entrance-left",
    "zone": "lobby",
    "type": "pir",
    "online": true,
    "hopCount": 2,
    "uptime": 3600,
    "lastSeen": "2026-06-29T14:32:00Z"
  }
]
```

#### Zones

```
GET    /api/v1/zones
POST   /api/v1/zones               {"name":"lobby"}
PATCH  /api/v1/zones/{id}
DELETE /api/v1/zones/{id}
POST   /api/v1/zones/{id}/command  {"action":"trigger", "params":{...}}
```

#### Events (SSE)

```
GET /api/v1/events
Accept: text/event-stream
Authorization: Bearer <api-key>
```

Event types:
```
event: motion
data: {"nodeId":7,"name":"entrance-left","zone":"lobby","hopCount":2,"timestamp":"..."}

event: node_online
data: {"nodeId":7,"name":"entrance-left"}

event: node_offline
data: {"nodeId":7,"name":"entrance-left","lastSeen":"..."}

event: enrolled
data: {"nodeId":7,"name":"entrance-left","type":"pir"}

event: health
data: {"nodeId":7,"name":"entrance-left","online":true,"uptime":3600,"hopCount":2}
```

#### Enrollment

```
GET  /api/v1/enrollments/pending
GET  /api/v1/enrollments
POST /api/v1/enrollments/{mac}/approve  {"name":"...","zone":"...","type":"pir","nodeId":7}
POST /api/v1/enrollments/{mac}/reject
```

#### Status

```
GET /api/v1/status
```
```json
{
  "serial": {"primary": "connected", "secondary": "connected"},
  "nodes": {"total": 12, "online": 11, "offline": 1},
  "mesh": {"masterOnline": true}
}
```

#### Server translation responsibility

| Artist sends | Server maps to |
|---|---|
| `"type": "pir"` | `adapter_types::PIR_ADAPTER = 0` |
| `"type": "led"` | `adapter_types::LED_ADAPTER = 2` |
| Node ID `7` | MAC from node registry |
| `{"action":"trigger","params":{}}` | Per-adapter command byte encoding |
| `POST /zones/lobby/command` | Fan-out to all node MACs in zone |

Internal command byte convention: `data[0]` = command ID, `data[1..63]` = parameters. Each adapter type documents its command IDs. Artists never see this.

The existing `/nodes`, `/broadcast`, `/health/request`, etc. endpoints remain for internal ops/debugging. The `/api/v1/` routes are the artist-facing interface.

### 5.12 Online timeout fix (server)

**File:** `server/orchestrator/mesh/api.go:240`

Change `GetOnlineNodes(30 * time.Second)` → `GetOnlineNodes(75 * time.Second)` (2.5× the 30s health interval). A single missed report no longer marks a node offline.

### 5.13 JOIN_ACK MAC field fix (server + firmware)

**Server** `server.go:435`: use `TargetMacAddress: node.MAC[:]` instead of `OriginMacAddress`.

**Firmware** `Serial_Adapter::handleCompleteFrame()` for JOIN_ACK: read `msg.targetMacAddress` instead of `msg.originMacAddress` when calling `enrollPeer()`. Update `decodeMeshMessage()` to copy `pbMsg.targetMacAddress` → `outMsg.targetMacAddress` for JOIN_ACK messages.

### 5.14 Remove EEPROM wipe on WDT loop (firmware)

**File:** `main/main.ino:105-108`

Replace:
```cpp
em.clearAll();
while (true) { delay(1000); }
```
With:
```cpp
Serial.println("[BOOT] WDT loop — halting. Manual reset required.");
while (true) { delay(1000); }
```

Operator performs manual reset if needed. Physical reset button sequence (existing) handles factory reset.

### 5.15 Decouple Mesh from Serial_Adapter (firmware)

Replace static call `Serial_Adapter::relayEnrollmentToServer()` in `Mesh.cpp` with a registered callback:

```cpp
// Mesh.h
typedef void (*EnrollmentRelayFn)(const uint8_t mac[6], const uint8_t pubKey[32]);
void setEnrollmentRelayFn(EnrollmentRelayFn fn);

// main.ino setup()
mesh.setEnrollmentRelayFn(Serial_Adapter::relayEnrollmentToServer);
```

Removes `#include "Serial_Adapter/Serial_Adapter.h"` from `Mesh.cpp`.

---

## 6. WiFi Fallback — Spec Only (Option C, not implemented)

Specced for future implementation. No implementation until explicitly prioritised.

### Constraint

ESP32 shares one radio between ESP-NOW and WiFi-STA. STA joining an AP switches the chip to the AP's channel. The WiFi AP **must** be configured for the same channel as `WIFI_CHANNEL` in `project_config.h` (default: channel 1). This is a deployment constraint.

### Approach: Parallel (always-on both)

Master sends every outgoing frame over serial AND WebSocket simultaneously. Server receives on both, deduplicates via existing replay cache (epoch+seq). No switchover logic, no failure detection state machine.

Master is USB-powered — always-on WiFi draws no concern.

### Firmware spec

`project_config.h` additions:
```cpp
constexpr bool WIFI_FALLBACK_ENABLED = false;  // opt-in per deployment
constexpr char WIFI_SSID[]           = "";     // AP must be on WIFI_CHANNEL
constexpr char WIFI_PASS[]           = "";
constexpr char WIFI_SERVER_HOST[]    = "";     // server IP or hostname
constexpr uint16_t WIFI_WS_PORT      = 8081;
```

When `WIFI_FALLBACK_ENABLED`:
- After mesh init, connect WiFi STA (same channel already set by ESP-NOW init)
- Open WebSocket to `ws://WIFI_SERVER_HOST:WIFI_WS_PORT/ws`
- Every frame written to serial also written to WebSocket binary (identical 2-byte LE length + protobuf payload format)
- Every frame received on WebSocket fed into `handleCompleteFrame()` (same path as serial)
- Non-blocking reconnect loop in `Serial_Adapter::loop()` on WiFi/WS disconnect

### Server spec

New WebSocket endpoint: `ws://:8081/ws` (separate port from HTTP API).

`WebSocketComm` struct mirrors `SerialComm`:
- `ReadFrame() (*MeshMessage, error)` — binary WebSocket frame, same 2-byte LE length + protobuf decode
- `WriteFrame(*MeshMessage) error` — same encode path as serial

`MeshServer` gains optional `wsComm *WebSocketComm`. Both `serialComm` and `wsComm` feed into the same `messageProcessor()`. Replay cache handles deduplication transparently.

Outgoing messages: sent on both serial and WebSocket when both connected. Firmware replay cache handles the duplicate command received over both radio and WebSocket.

New env var: `WS_PORT=8081` (only required when WiFi fallback nodes deployed).

---

## 7. EEPROM Layout (updated)

```
  0   MASTER_FLAG        (1 byte)
  1   DEV_FLAG           (1 byte)
  8   ADAPTER_TYPE       (1 byte)
 16   MESH_KEY           (16 bytes, ends 31)
 32   PEER_LIST          (380 bytes: 10 × 38B, ends 411)
412   REBOOT_REASON      (1 byte)
413   REBOOT_COUNT       (1 byte)
414   RESERVED           (3 bytes)
417   PRIVATE_KEY        (32 bytes, ends 448)
449   PUBLIC_KEY         (32 bytes, ends 480)
481   KEYPAIR_CRC        (2 bytes, ends 482)
483   ENROLLED_FLAG      (1 byte)
484   BOOT_EPOCH         (4 bytes, ends 487)
488   KNOWN_MASTER_MAC   (6 bytes, ends 493)
494   SCHEMA_VERSION     (1 byte)  → bump to 3
495   TX_POWER_PRESET    (1 byte)
496   NODE_ID            (1 byte)  ← NEW: server-assigned logical ID (0=unassigned)
497   DUAL_MASTER_FLAG   (1 byte)  ← NEW: 0x01 = this node is a known dual-master pair
```

Total used: 498 bytes — fits in 512.

Bump `CURRENT_SCHEMA_VERSION` to 3. Add migration in `EEPROM_Manager::init()`: v2→v3 writes 0x00 to NODE_ID and DUAL_MASTER_FLAG addresses.

---

## 8. Wire Protocol Change (PROTO_VERSION 2)

`mesh_message.data` grows from 12 to 64 bytes. `sizeof(mesh_message)` changes from 75 to 127 bytes.

```
Before: 1+1+4+6+6+6+12+1+4+2+32 = 75 bytes
After:  1+1+4+6+6+6+64+1+4+2+32 = 127 bytes
```

`PROTO_VERSION` bumps from 1 to 2. Server drops messages with `ProtoVersion > 0 && ProtoVersion != 2`.

Nanopb `.options` for `data` field: `max_size:64`.

The 3-chunk enrollment workaround is removed simultaneously. Single `MESH_TYPE_ENROLLMENT` message with `enrollmentPublicKey[32]` (unchanged) carries the full key.

---

## 9. Implementation Order

| Phase | Changes | Risk |
|---|---|---|
| 1 — Fix routing | 5.1, 5.2, 5.3 (multi-hop relay) | High — test dedup carefully |
| 2 — Fix bugs | 5.7 (TX power), 5.14 (WDT), 5.12 (timeout), 5.13 (JOIN_ACK field) | Low |
| 3 — Proto v2 | 5.4, 5.5 (64-byte data, remove chunking) | Medium — coordinated firmware+server deploy |
| 4 — Visibility | 5.6 (PIR health heartbeat) | Low |
| 5 — Node identity | 5.8 (logical ID, 7-seg states, EEPROM v3) | Medium |
| 6 — Artist API | 5.11 (v1 API layer), 5.3 (SSE events) | Medium |
| 7 — Hotswap | 5.9 (hotswap workflow) | Medium — depends on Phase 5+6 |
| 8 — Dual master | 5.10 (Option B) | High — mesh behaviour change |
| 9 — Cleanup | 5.15 (decouple Mesh/Serial_Adapter) | Low |
| Future | 6 (WiFi fallback / Option C) | High — deployment constraint |
