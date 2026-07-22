# Phase 3 Proto V2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand `mesh_message.data` from 12 to 64 bytes, bump `PROTO_VERSION` to 2, and remove the 3-chunk enrollment workaround that only existed because 32-byte keys couldn't fit in 12 bytes.

**Architecture:** Two tasks — one per repo. Firmware (Task 1) changes the on-air struct layout, proto options, version constant, and enrollment send/receive. Server (Task 2) updates its data length constant and proto version guard. Both must be deployed together since `PROTO_VERSION` 1 ↔ 2 are incompatible; after Task 2, the server drops firmware v1 messages and vice versa.

**Tech Stack:** C++17 / Arduino ESP32 (firmware), nanopb-0.4.9.1 (protobuf), GoogleTest + CMake (firmware tests), Go 1.26 + `go test` (server tests).

## Global Constraints

- Firmware repo root: `/Users/benjamin.swanepoel/projects/personal/Planetopia-nodes`
- Server repo root: `/Users/benjamin.swanepoel/projects/personal/motionSensorServer`
- Both repos: branch `feat/phase3-proto-v2` — do NOT commit to `main`
- Git identity: already configured to `49689582+superbrobenji@users.noreply.github.com` on both repos — do not change it
- Firmware test command (from repo root): `cmake -B tests/build -S tests && cmake --build tests/build && ctest --test-dir tests/build --output-on-failure`
- Server test command (from `server/orchestrator/`): `GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/...`
- nanopb generator: `python3 /Users/benjamin.swanepoel/Library/Python/3.9/lib/python/site-packages/nanopb/generator/nanopb_generator.py`
- `Serial_Adapter.cpp` and `PIR_Adapter.cpp` are NOT compiled in the firmware unit test build — changes there are verified by building the test suite without errors (CMake picks them up as source targets)
- No new abstractions beyond what the spec requires; no refactoring unrelated to the data-field expansion

---

### Task 1: Firmware — data[64], PROTO_VERSION 2, single-message enrollment

**Scope:** Grow `mesh_message.data[12]` → `data[64]`, bump `static_assert` and `PROTO_VERSION`, update the `.options` file and regenerate nanopb, update every `data[12]` signature and buffer declaration, replace 3-chunk enrollment send+receive with a single message using the existing `enrollmentPublicKey[32]` field.

**Files:**
- Modify: `main/proto/mesh.options`
- Regenerate: `main/src/Mesh/serialization/mesh.pb.h` and `mesh.pb.c`
- Modify: `main/src/Mesh/Mesh.h` (struct field, static_assert, PROTO_VERSION, method signatures)
- Modify: `main/src/Mesh/Mesh.cpp` (method signatures, sendEnrollmentRequest, processEnrollmentRequest)
- Modify: `main/src/Adapter/Adapter.h` (sendDataThroughMesh signature)
- Modify: `main/src/Adapter/Adapter.cpp` (sendDataThroughMesh signature)
- Modify: `main/src/Adapter/PIR_Adapter/PIR_Adapter.h` (sendDataTrampoline signature)
- Modify: `main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp` (signature + local buffer)
- Modify: `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` (local buffer)
- Modify: `tests/mocks/firmware_stubs.cpp` (buildMessage stub signature; remove processEnrollmentRequest stub)
- Modify: `tests/mocks/mesh_logic_impl.cpp` (transmitCore + broadcastAdapterData signatures; add sendEnrollmentRequest + processEnrollmentRequest real impls)
- Modify: `tests/unit/test_mesh_logic.cpp` (kPayload[12] → [64]; add EnrollmentTest)

**Interfaces:**
- Consumes: nothing from Task 2
- Produces: `PROTO_VERSION = 2` constant; `sizeof(mesh_message) == 127`; `sendEnrollmentRequest()` sends exactly 1 ESP-NOW packet; `processEnrollmentRequest()` sets `_pendingEnrollmentRelay`, `_pendingEnrollmentMac`, `_pendingEnrollmentPubKey` in one call

---

- [ ] **Step 1: Update mesh.options max_size**

In `main/proto/mesh.options`, change line 4:

```
# Before:
mesh.MeshMessage.data              max_size:12

# After:
mesh.MeshMessage.data              max_size:64
```

Full file after edit:
```
mesh.MeshMessage.originMacAddress  max_size:6  fixed_length:true
mesh.MeshMessage.targetMacAddress  max_size:6  fixed_length:true
mesh.MeshMessage.lastHopMacAddress max_size:6  fixed_length:true
mesh.MeshMessage.data              max_size:64
mesh.MeshMessage.public_key        max_size:32
```

- [ ] **Step 2: Regenerate mesh.pb.h and mesh.pb.c**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
python3 /Users/benjamin.swanepoel/Library/Python/3.9/lib/python/site-packages/nanopb/generator/nanopb_generator.py \
  --output-dir main/src/Mesh/serialization/ \
  main/proto/mesh.proto
```

Expected: command exits 0, no error output. Then verify:
```bash
grep "PB_BYTES_ARRAY_T(64)" main/src/Mesh/serialization/mesh.pb.h
```
Expected: `typedef PB_BYTES_ARRAY_T(64) mesh_MeshMessage_data_t;`

- [ ] **Step 3: Update struct field, static_assert, and PROTO_VERSION in Mesh.h**

In `main/src/Mesh/Mesh.h`:

Line 38 — bump PROTO_VERSION:
```cpp
// Before:
static constexpr uint8_t PROTO_VERSION = 1;
// After:
static constexpr uint8_t PROTO_VERSION = 2;
```

Line 42 — update comment:
```cpp
// Before:
  uint8_t protoVersion; // Always PROTO_VERSION (1)
// After:
  uint8_t protoVersion; // Always PROTO_VERSION (2)
```

Line 48 — grow data field:
```cpp
// Before:
  uint8_t data[12];
// After:
  uint8_t data[64];
```

Line 55 — update static_assert:
```cpp
// Before:
static_assert(sizeof(mesh_message) == 75, "mesh_message size changed — update server proto");
// After:
static_assert(sizeof(mesh_message) == 127, "mesh_message size changed — update server proto");
```

- [ ] **Step 4: Update method signatures in Mesh.h**

Still in `main/src/Mesh/Mesh.h`, update these five signatures (change `data[12]` → `data[64]` in each):

Line 104:
```cpp
// Before:
  mesh_message buildMessage(adapter_types type, const uint8_t data[12], MeshMessageType msgType);
// After:
  mesh_message buildMessage(adapter_types type, const uint8_t data[64], MeshMessageType msgType);
```

Line 132:
```cpp
// Before:
  void transmitCore(const adapter_types type, const uint8_t data[12],
// After:
  void transmitCore(const adapter_types type, const uint8_t data[64],
```

Line 216:
```cpp
// Before:
  static void transmit(const adapter_types type, const uint8_t data[12]);
// After:
  static void transmit(const adapter_types type, const uint8_t data[64]);
```

Line 240:
```cpp
// Before:
  void broadcastAdapterData(adapter_types type, const uint8_t data[12]);
// After:
  void broadcastAdapterData(adapter_types type, const uint8_t data[64]);
```

Line 243:
```cpp
// Before:
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t data[12]);
// After:
  static void broadcastAdapterDataStatic(adapter_types type, const uint8_t data[64]);
```

- [ ] **Step 5: Update method signatures in Mesh.cpp**

In `main/src/Mesh/Mesh.cpp`, change the following function definition signatures (`data[12]` → `data[64]`):

Line 364:
```cpp
// Before:
mesh_message Mesh::buildMessage(adapter_types type, const uint8_t data[12],
// After:
mesh_message Mesh::buildMessage(adapter_types type, const uint8_t data[64],
```

Line 652:
```cpp
// Before:
void Mesh::transmitCore(const adapter_types type, const uint8_t data[12], MeshMessageType msgType,
// After:
void Mesh::transmitCore(const adapter_types type, const uint8_t data[64], MeshMessageType msgType,
```

Line 681:
```cpp
// Before:
void Mesh::transmit(const adapter_types type, const uint8_t data[12]) {
// After:
void Mesh::transmit(const adapter_types type, const uint8_t data[64]) {
```

Line 791:
```cpp
// Before:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[12]) {
// After:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[64]) {
```

Line 797:
```cpp
// Before:
void Mesh::broadcastAdapterDataStatic(adapter_types type, const uint8_t data[12]) {
// After:
void Mesh::broadcastAdapterDataStatic(adapter_types type, const uint8_t data[64]) {
```

- [ ] **Step 6: Replace sendEnrollmentRequest in Mesh.cpp**

In `main/src/Mesh/Mesh.cpp`, replace the entire function from line 984 (`void Mesh::sendEnrollmentRequest()`) through line 1014 (the closing `}`):

```cpp
void Mesh::sendEnrollmentRequest() {
  mesh_message msg = {};
  msg.messageType = MESH_TYPE_ENROLLMENT;
  msg.dataType = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.originMacAddress, deviceMacAddress, 6);
  memset(msg.targetMacAddress, 0xFF, 6);
  memcpy(msg.lastHopMacAddress, deviceMacAddress, 6);
  msg.hopCount = 0;
  memcpy(msg.enrollmentPublicKey, devicePublicKey, 32);

  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  Logger::logln("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}
```

- [ ] **Step 7: Replace processEnrollmentRequest in Mesh.cpp**

In `main/src/Mesh/Mesh.cpp`, replace the entire function from line 1016 (`void Mesh::processEnrollmentRequest(const mesh_message& msg)`) through line 1081 (the closing `}`):

```cpp
void Mesh::processEnrollmentRequest(const mesh_message& msg) {
  if (!isMaster) {
    return;
  }
  memcpy(_pendingEnrollmentMac, msg.originMacAddress, 6);
  memcpy(_pendingEnrollmentPubKey, msg.enrollmentPublicKey, 32);
  _pendingEnrollmentRelay = true;
  Logger::logln("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}
```

- [ ] **Step 8: Update Adapter.h and Adapter.cpp signatures**

In `main/src/Adapter/Adapter.h` line 40:
```cpp
// Before:
                           const uint8_t data[12]); // sends data through mesh
// After:
                           const uint8_t data[64]); // sends data through mesh
```

In `main/src/Adapter/Adapter.cpp` line 23:
```cpp
// Before:
void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[12]) {
// After:
void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[64]) {
```

- [ ] **Step 9: Update PIR_Adapter signatures and buffer**

In `main/src/Adapter/PIR_Adapter/PIR_Adapter.h` line 20:
```cpp
// Before:
  static void sendDataTrampoline(adapter_types adapterType, uint8_t data[12]);
// After:
  static void sendDataTrampoline(adapter_types adapterType, uint8_t data[64]);
```

In `main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp` line 48:
```cpp
// Before:
void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[12]) {
// After:
void PIR_Adapter::sendDataTrampoline(adapter_types adapterType, uint8_t data[64]) {
```

In `main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp` line 77:
```cpp
// Before:
    uint8_t data[12] = {1};
// After:
    uint8_t data[64] = {1};
```

- [ ] **Step 10: Update Serial_Adapter local buffer**

In `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` line 27:
```cpp
// Before:
  uint8_t data[12] = {0};
// After:
  uint8_t data[64] = {0};
```

- [ ] **Step 11: Update firmware_stubs.cpp**

In `tests/mocks/firmware_stubs.cpp`:

Line 92 — update buildMessage stub signature:
```cpp
// Before:
mesh_message Mesh::buildMessage(adapter_types, const uint8_t[12], MeshMessageType) {
// After:
mesh_message Mesh::buildMessage(adapter_types, const uint8_t[64], MeshMessageType) {
```

Line 121 — **remove** the `processEnrollmentRequest` stub (real implementation moves to mesh_logic_impl.cpp in Step 12):
```cpp
// Remove this entire line:
void Mesh::processEnrollmentRequest(const mesh_message&) {}
```

And **remove** the `sendEnrollmentRequest` stub at line 80 (also moves to mesh_logic_impl.cpp):
```cpp
// Remove this entire line:
void Mesh::sendEnrollmentRequest() {}
```

Update the comment block that follows (around line 125) to reflect the moves:
```cpp
// sendEnrollmentRequest and processEnrollmentRequest are implemented in mesh_logic_impl.cpp (real logic)
// processJoinAck is implemented in mesh_logic_impl.cpp (real logic)
```

- [ ] **Step 12: Add real enrollment implementations to mesh_logic_impl.cpp**

In `tests/mocks/mesh_logic_impl.cpp`, update the two function signatures that contain `data[12]` and add the real enrollment implementations at the end of the `mesh` namespace block.

First, update `transmitCore` signature at line 195:
```cpp
// Before:
void Mesh::transmitCore(const adapter_types type, const uint8_t data[12], MeshMessageType msgType,
// After:
void Mesh::transmitCore(const adapter_types type, const uint8_t data[64], MeshMessageType msgType,
```

Update `broadcastAdapterData` signature at line 264:
```cpp
// Before:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[12]) {
// After:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[64]) {
```

Then append the following two real implementations at the end of the `mesh` namespace block (before the closing `}  // namespace mesh`):

```cpp
void Mesh::sendEnrollmentRequest() {
  mesh_message msg = {};
  msg.messageType = MESH_TYPE_ENROLLMENT;
  msg.dataType = adapter_types::UNKNOWN_ADAPTER;
  memcpy(msg.originMacAddress, deviceMacAddress, 6);
  memset(msg.targetMacAddress, 0xFF, 6);
  memcpy(msg.lastHopMacAddress, deviceMacAddress, 6);
  msg.hopCount = 0;
  memcpy(msg.enrollmentPublicKey, devicePublicKey, 32);

  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMac, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  Logger::logln("MESH", "Enrollment request sent", LogLevel::LOG_INFO);
}

void Mesh::processEnrollmentRequest(const mesh_message& msg) {
  if (!isMaster) {
    return;
  }
  memcpy(_pendingEnrollmentMac, msg.originMacAddress, 6);
  memcpy(_pendingEnrollmentPubKey, msg.enrollmentPublicKey, 32);
  _pendingEnrollmentRelay = true;
  Logger::logln("MESH", "Enrollment request received, deferring relay to loop()",
                LogLevel::LOG_INFO);
}
```

- [ ] **Step 13: Write the failing enrollment tests**

In `tests/unit/test_mesh_logic.cpp`, add the following test fixture and two tests after the existing `DrainRecvQueueTest` block (before the final `}` of the file):

```cpp
// ─── EnrollmentTest ──────────────────────────────────────────────────────────

class EnrollmentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    resetEspNowMockState();
    resetEEPROMMockState();
    espNowSentPackets.clear();
  }
};

TEST_F(EnrollmentTest, SendsSingleEspNowMessage) {
  Mesh mesh;
  static constexpr uint8_t kPubKey[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20
  };
  memcpy(mesh.devicePublicKey, kPubKey, 32);

  mesh.sendEnrollmentRequest();

  ASSERT_EQ(espNowSentPackets.size(), 1u)
      << "Expected exactly 1 ESP-NOW packet (was 3 with old chunking)";
  const auto& pkt = espNowSentPackets[0];
  ASSERT_GE(pkt.data.size(), sizeof(mesh_message));
  const mesh_message* sent = reinterpret_cast<const mesh_message*>(pkt.data.data());
  EXPECT_EQ(sent->messageType, MESH_TYPE_ENROLLMENT);
  EXPECT_EQ(memcmp(sent->enrollmentPublicKey, kPubKey, 32), 0)
      << "Full public key must be present in a single message";
}

TEST_F(EnrollmentTest, ProcessSingleMessageSetsKey) {
  Mesh mesh;
  mesh.isMaster = true;

  mesh_message msg = {};
  msg.messageType = MESH_TYPE_ENROLLMENT;
  static constexpr uint8_t kMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  static constexpr uint8_t kKey[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20
  };
  memcpy(msg.originMacAddress, kMac, 6);
  memcpy(msg.enrollmentPublicKey, kKey, 32);

  mesh.processEnrollmentRequest(msg);

  EXPECT_TRUE(mesh._pendingEnrollmentRelay);
  EXPECT_EQ(memcmp(mesh._pendingEnrollmentMac, kMac, 6), 0);
  EXPECT_EQ(memcmp(mesh._pendingEnrollmentPubKey, kKey, 32), 0)
      << "Full 32-byte key must be copied without chunk reassembly";
}
```

- [ ] **Step 14: Update kPayload in existing test**

In `tests/unit/test_mesh_logic.cpp` line 350, change:
```cpp
// Before:
  static constexpr uint8_t kPayload[12] = {0x01,0x02,0x03,0,0,0,0,0,0,0,0,0};
// After:
  static constexpr uint8_t kPayload[64] = {0x01,0x02,0x03};
```

(Zero-initialisation fills the remaining 61 bytes with 0x00.)

- [ ] **Step 15: Run tests to verify they fail before build**

```bash
cd /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes
cmake -B tests/build -S tests && cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: compilation succeeds. The two new `EnrollmentTest` tests should PASS immediately (the real implementations are already added to `mesh_logic_impl.cpp`). If they FAIL, check for compilation errors first.

Expected total: **49/49 tests pass** (47 existing + 2 new enrollment tests).

- [ ] **Step 16: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes \
  add main/proto/mesh.options \
      main/src/Mesh/serialization/mesh.pb.h \
      main/src/Mesh/serialization/mesh.pb.c \
      main/src/Mesh/Mesh.h \
      main/src/Mesh/Mesh.cpp \
      main/src/Adapter/Adapter.h \
      main/src/Adapter/Adapter.cpp \
      main/src/Adapter/PIR_Adapter/PIR_Adapter.h \
      main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp \
      main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp \
      tests/mocks/firmware_stubs.cpp \
      tests/mocks/mesh_logic_impl.cpp \
      tests/unit/test_mesh_logic.cpp
git -C /Users/benjamin.swanepoel/projects/personal/Planetopia-nodes \
  commit -m "feat(firmware): proto v2 — data[64], PROTO_VERSION 2, single-message enrollment"
```

---

### Task 2: Server — MaxDataLength 64 + proto version guard v2

**Scope:** Update `MaxDataLength = 12` → `64`, bump the proto version guard from v1 to v2, add tests that cover both changes.

**Files:**
- Modify: `server/orchestrator/mesh/constants.go`
- Modify: `server/orchestrator/mesh/server.go` (two proto-version check lines)
- Modify: `server/orchestrator/mesh/server_test.go` (add proto version guard test)

**Interfaces:**
- Consumes: nothing from Task 1
- Produces: `MaxDataLength = 64`; `handleMessage` drops v1 messages; `handleMessage` processes v2 messages

---

- [ ] **Step 1: Write the failing proto version guard test**

In `server/orchestrator/mesh/server_test.go`, add the following test after `TestSetTxPowerPreset_InvalidPreset_ReturnsError`:

```go
func TestHandleMessage_ProtoVersionGuard(t *testing.T) {
	ms := newTestMeshServer(t)
	mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}

	// Build a health report payload that would register a node if processed.
	healthData := make([]byte, 12)
	healthData[0] = byte(OpHealthReport)
	healthData[1] = byte(AdapterTypePIR)
	copy(healthData[2:8], mac)
	// bytes 8-11: uptime = 0 (zero value)

	// v1 message: current guard accepts v1 — after fix it must drop v1.
	v1msg := &MeshMessage{
		ProtoVersion:     1,
		MessageType:      MessageTypeAdapterData,
		DataType:         AdapterTypeSerial,
		Data:             healthData,
		OriginMacAddress: mac,
	}
	if err := ms.handleMessage(v1msg); err != nil {
		t.Fatalf("handleMessage(v1) returned unexpected error: %v", err)
	}
	if _, ok := ms.GetNodeRegistry().GetNode(mac); ok {
		t.Error("v1 message should be dropped — node must not be registered")
	}

	// v2 message: must be accepted and processed.
	v2msg := &MeshMessage{
		ProtoVersion:     2,
		MessageType:      MessageTypeAdapterData,
		DataType:         AdapterTypeSerial,
		Data:             healthData,
		OriginMacAddress: mac,
	}
	if err := ms.handleMessage(v2msg); err != nil {
		t.Fatalf("handleMessage(v2) returned unexpected error: %v", err)
	}
	if _, ok := ms.GetNodeRegistry().GetNode(mac); !ok {
		t.Error("v2 message must be processed — node should be registered after health report")
	}
}
```

- [ ] **Step 2: Run to verify the test fails**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestHandleMessage_ProtoVersionGuard -v
```

Expected: `TestHandleMessage_ProtoVersionGuard` FAIL — the v1 message currently passes the guard (the guard accepts v1), so the node IS registered, failing the "must not be registered" assertion.

- [ ] **Step 3: Update MaxDataLength in constants.go**

In `server/orchestrator/mesh/constants.go` line 48:
```go
// Before:
const MaxDataLength = 12
// After:
const MaxDataLength = 64
```

- [ ] **Step 4: Update proto version guard in server.go**

In `server/orchestrator/mesh/server.go`, update two lines:

Line 249:
```go
// Before:
	if msg.ProtoVersion > 0 && msg.ProtoVersion != 1 {
		slog.Warn("Unsupported proto version — dropping", "version", msg.ProtoVersion, "origin", fmt.Sprintf("%x", msg.OriginMacAddress))
		return nil
	}
// After:
	if msg.ProtoVersion > 0 && msg.ProtoVersion != 2 {
		slog.Warn("Unsupported proto version — dropping", "version", msg.ProtoVersion, "origin", fmt.Sprintf("%x", msg.OriginMacAddress))
		return nil
	}
```

Line 255:
```go
// Before:
	if msg.ProtoVersion == 1 && msg.EpochNum > 0 {
// After:
	if msg.ProtoVersion == 2 && msg.EpochNum > 0 {
```

- [ ] **Step 5: Run to verify the proto version test now passes**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/... -run TestHandleMessage_ProtoVersionGuard -v
```

Expected: PASS.

- [ ] **Step 6: Run full server test suite**

```bash
cd /Users/benjamin.swanepoel/projects/personal/motionSensorServer/server/orchestrator
GOROOT=/opt/homebrew/Cellar/go/1.26.4/libexec /opt/homebrew/bin/go test ./mesh/...
```

Expected: all tests pass, 0 failures.

Note: `TestSetTxPowerPreset_SendsProtoFrame` builds a `make([]byte, MaxDataLength)` payload. After the constant change to 64, the payload becomes 64 bytes. The test asserts `msg.Data[0] == OpTxPowerSet` and `msg.Data[1] == 1` — both remain valid since Go zero-initialises the remaining bytes. The test should continue to pass.

- [ ] **Step 7: Commit**

```bash
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer \
  add server/orchestrator/mesh/constants.go \
      server/orchestrator/mesh/server.go \
      server/orchestrator/mesh/server_test.go
git -C /Users/benjamin.swanepoel/projects/personal/motionSensorServer \
  commit -m "feat(server): proto v2 — MaxDataLength 64, version guard accepts v2 only"
```

---

## Self-Review

**Spec coverage check:**

| Spec requirement | Task | Notes |
|---|---|---|
| `mesh_message.data[12]` → `data[64]` | Task 1 step 3–10 | struct + all signatures |
| `static_assert(sizeof(mesh_message) == 75)` → `== 127` | Task 1 step 3 | ✓ |
| `mesh.proto` data field `max_size:64` | Task 1 step 1–2 | regenerated from options |
| `PROTO_VERSION = 2` | Task 1 step 3 | Mesh.h line 38 |
| Server `MaxDataLength = 64` | Task 2 step 3 | ✓ |
| Proto version guard drop v1 / accept v2 | Task 2 step 4 | two lines in server.go |
| Remove 3-chunk enrollment send | Task 1 step 6 | sendEnrollmentRequest |
| Remove chunk accumulation receive | Task 1 step 7 | processEnrollmentRequest |
| `ParseHealthReport` bounds check unchanged | not modified | spec says "still uses first 12 bytes, unchanged" — `len(msg.Data) < 12` stays |

**Placeholder scan:** No TBDs. All steps contain exact code.

**Type consistency:**
- `mesh_message.data[64]` ← used throughout; static_assert confirms 127 bytes
- `PROTO_VERSION = 2` ← referenced in Mesh.cpp guard and set in Mesh.h
- `MaxDataLength = 64` ← server; `SetTxPowerPreset` test uses `MaxDataLength` by name (no hardcoded 12)
- `_pendingEnrollmentRelay / _pendingEnrollmentMac / _pendingEnrollmentPubKey` ← accessed in tests via UNIT_TEST public exposure (Mesh.h:72–79)
