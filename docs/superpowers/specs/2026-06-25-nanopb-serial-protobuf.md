# Firmware Spec: nanopb Serial Protobuf Migration

**Date:** 2026-06-25
**Repo:** planetopia-nodes (branch off `main` after firmware-overhaul merge)
**Paired server change:** motionSensorServer — add fields 8-11 to proto + enrollment handler

---

## Goal

Replace the hand-rolled Protobuf encode/decode in `Serial_Adapter.cpp` (and the dead `ProtobufCodec.cpp`) with **nanopb** — the standard embedded Protobuf library for ESP32/Arduino. Fix the enrollment relay to send proper Protobuf frames instead of raw binary bytes, which currently breaks the server's `proto.Unmarshal` decoder.

No behaviour visible to the mesh (ESP-NOW layer) changes. Only the serial wire format for enrollment frames changes.

---

## Background / Why Now

1. **ProtobufCodec.cpp** — exists in `main/src/Mesh/serialization/` but is never used. Dead code compiled into every test binary.
2. **Serial_Adapter inline codec** — duplicates ProtobufCodec with its own hand-rolled varint/zigzag/bytes helpers. Two implementations of the same thing, neither with test coverage for edge cases.
3. **`relayEnrollmentToServer()`** — sends raw binary `[0xC0][6B mac][32B pubkey]` with a 2-byte length prefix. The server reads 2-byte length → tries `proto.Unmarshal(39 bytes starting with 0xC0)` → fails silently (treats as unknown PIR event). **Enrollment is currently silently broken.**

---

## Proto Schema (source of truth)

Both firmware and server must use this schema.

**`main/proto/mesh.proto`** (create this file):

```proto
syntax = "proto3";
package mesh;

message MeshMessage {
  uint32 messageType       = 1;
  // 0 = ADAPTER_DATA      (device→server: normal adapter data)
  // 1 = MASTER_BEACON     (device→server: mesh heartbeat)
  // 2 = ENROLLMENT        (device→server: node requests to join)
  // 3 = SERIAL_CMD_BROADCAST (server→device: broadcast adapter data command)
  // 4 = JOIN_ACK          (server→device: enrollment approved/rejected)
  //
  // NOTE: 3 is reserved for server→device commands (matches server's
  // MessageTypeSerialCmdBroadcast = 3). JOIN_ACK is 4 to avoid collision.

  sint32 dataType          = 2;  // zigzag: -1=UNKNOWN, 0=PIR, 1=WIFI, 2=LED, 3=SERIAL
  bytes  originMacAddress  = 3;  // 6 bytes
  bytes  targetMacAddress  = 4;  // 6 bytes
  bytes  lastHopMacAddress = 5;  // 6 bytes
  bytes  data              = 6;  // up to 12 bytes of adapter payload
  uint32 hopCount          = 7;
  uint32 epochNum          = 8;  // sender boot epoch (replay protection)
  uint32 seqNum            = 9;  // per-boot sequence counter (replay protection)
  uint32 protoVersion      = 10; // must be 1 for v1 messages
  bytes  public_key        = 11; // optional: 32-byte Curve25519 key (ENROLLMENT/JOIN_ACK only)
}
```

**`main/proto/mesh.options`** (nanopb field size annotations):

```
MeshMessage.originMacAddress  max_size:6  fixed_length:true
MeshMessage.targetMacAddress  max_size:6  fixed_length:true
MeshMessage.lastHopMacAddress max_size:6  fixed_length:true
MeshMessage.data              max_size:12
MeshMessage.public_key        max_size:32
```

`fixed_length:true` fields generate `pb_byte_t field[N]` (always-present fixed array).
`data` without `fixed_length` generates `uint8_t data[12]; pb_size_t data_count;` — set `data_count` to the real payload length, not always 12.
`public_key` without `fixed_length` generates `bool has_public_key; uint8_t public_key[32];` — only set `has_public_key = true` when encoding enrollment/join_ack with a real key. This avoids sending 34 wasted bytes (tag+len+32 zeros) on every PIR/health/beacon message.

### Message type values in firmware

Add to `Mesh.h` (the `MeshMessageType` enum):
```cpp
MESH_TYPE_ADAPTER_DATA      = 0,
MESH_TYPE_MASTER_BEACON     = 1,
MESH_TYPE_ENROLLMENT        = 2,
MESH_TYPE_SERIAL_CMD_BROADCAST = 3,  // server→device only; device never sends this
MESH_TYPE_JOIN_ACK          = 4,     // server→device only; was 3 in original spec — CHANGED to avoid collision
```

Update `SERIAL_MSG_BROADCAST = 3` in `Serial_Adapter.h` to `SERIAL_MSG_BROADCAST = 3` (unchanged — already correct, just add `SERIAL_MSG_JOIN_ACK = 4`).

---

## Server-Side Changes Required

The server (`motionSensorServer`) needs a paired update before enrollment works end-to-end. The firmware can land first (proto3 unknown fields are ignored by the current server, so existing ADAPTER_DATA and MASTER_BEACON flows are unaffected):

### `server/orchistrator/mesh/mesh.proto`
Add fields 8-11:
```proto
uint32 epochNum      = 8;
uint32 seqNum        = 9;
uint32 protoVersion  = 10;
bytes  public_key    = 11;
```
Add comment clarifying messageType=4 is JOIN_ACK.

### `server/orchistrator/mesh/constants.go`
```go
MessageTypeEnrollment        uint32 = 2
MessageTypeSerialCmdBroadcast uint32 = 3  // existing — unchanged
MessageTypeJoinAck           uint32 = 4  // new
```

### `server/orchistrator/mesh/server.go`
In `handleMessage()` switch:
```go
case MessageTypeEnrollment:
    return ms.handleEnrollment(msg)
```

`handleEnrollment()` should:
1. Extract `msg.OriginMacAddress` (6B) and `msg.PublicKey` (32B)
2. Call `nodeauth.Registry.Approve(mac, pubkey)` or `Reject(mac)` (the `nodeauth/` package already exists)
3. Build JOIN_ACK response:
   - Approved: `messageType=4, originMacAddress=mac, public_key=pubkey (non-zero)`
   - Rejected: `messageType=4, originMacAddress=mac, public_key` absent (has_public_key=false → server sends with empty bytes)
4. Call `ms.serialComm.WriteFrame(joinAck)`

### `server/orchistrator/mesh/mesh.pb.go`
Regenerate from updated `mesh.proto` using `protoc`.

---

## Toolchain Setup

### nanopb C runtime
Download nanopb 0.4.9.1 and copy these 7 files into `main/src/Mesh/serialization/nanopb/`:
- `pb.h`
- `pb_encode.h` + `pb_encode.c`
- `pb_decode.h` + `pb_decode.c`
- `pb_common.h` + `pb_common.c`

Source: `https://github.com/nanopb/nanopb/archive/refs/tags/0.4.9.1.tar.gz`

### Generate C files
```bash
python3 <nanopb_generator.py> \
  --proto-path=main/proto \
  --output-dir=main/src/Mesh/serialization \
  main/proto/mesh.proto
```

Produces (commit both):
- `main/src/Mesh/serialization/mesh.pb.h`
- `main/src/Mesh/serialization/mesh.pb.c`

---

## Files Changed (Firmware)

### Create

| File | Purpose |
|------|---------|
| `main/proto/mesh.proto` | Proto schema source of truth |
| `main/proto/mesh.options` | nanopb field size annotations |
| `main/src/Mesh/serialization/mesh.pb.h` | Generated nanopb header |
| `main/src/Mesh/serialization/mesh.pb.c` | Generated nanopb source |
| `main/src/Mesh/serialization/nanopb/pb.h` | nanopb runtime |
| `main/src/Mesh/serialization/nanopb/pb_encode.{h,c}` | nanopb encoder |
| `main/src/Mesh/serialization/nanopb/pb_decode.{h,c}` | nanopb decoder |
| `main/src/Mesh/serialization/nanopb/pb_common.{h,c}` | nanopb shared |

### Delete

| File | Reason |
|------|--------|
| `main/src/Mesh/serialization/ProtobufCodec.cpp` | Dead code — never called |
| `main/src/Mesh/serialization/ProtobufCodec.h` | Dead code |
| `tests/unit/test_protobuf_codec.cpp` | Tests dead code |

### Modify

| File | What changes |
|------|-------------|
| `main/src/Mesh/Mesh.h` | Add `enrollmentPublicKey[32]` to `mesh_message`; add `MESH_TYPE_SERIAL_CMD_BROADCAST=3`, `MESH_TYPE_JOIN_ACK=4` to enum |
| `main/src/Adapter/Serial_Adapter/Serial_Adapter.h` | Remove private codec helpers + old enrollment opcodes (0xC0-0xC2); add `SERIAL_MSG_JOIN_ACK=4`; add nanopb includes |
| `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` | Replace encodeMeshMessage/decodeMeshMessage/relayEnrollmentToServer; update handleCompleteFrame for JOIN_ACK |
| `tests/CMakeLists.txt` | Swap ProtobufCodec.cpp for nanopb + mesh.pb.c; remove test_protobuf_codec target |
| `tests/unit/test_serial_framing.cpp` | Update to test nanopb encode/decode round-trip |

---

## Detailed Changes

### `main/src/Mesh/Mesh.h` — mesh_message struct

Add `enrollmentPublicKey` at the end:

```cpp
struct mesh_message {
  uint8_t protoVersion;
  MeshMessageType messageType;
  adapter_types dataType;
  uint8_t originMacAddress[6];
  uint8_t targetMacAddress[6];
  uint8_t lastHopMacAddress[6];
  uint8_t data[12];
  uint8_t hopCount;
  uint32_t epochNum;
  uint16_t seqNum;
  uint8_t enrollmentPublicKey[32]; // zero for non-enrollment messages
};
// Update static_assert: sizeof changes from 43 to 75 bytes
static_assert(sizeof(mesh_message) == 75, "mesh_message struct size mismatch");
```

ESP-NOW sends this struct as raw bytes. 75 bytes is well within the 250-byte ESP-NOW limit. All `esp_now_send` call sites use `sizeof(mesh_message)` — no hardcoded size to update.

Update the `MeshMessageType` enum:
```cpp
enum class MeshMessageType : uint8_t {
  MESH_TYPE_ADAPTER_DATA         = 0,
  MESH_TYPE_MASTER_BEACON        = 1,
  MESH_TYPE_ENROLLMENT           = 2,
  MESH_TYPE_SERIAL_CMD_BROADCAST = 3,  // server→device only
  MESH_TYPE_JOIN_ACK             = 4,  // server→device only
};
```

### `main/src/Adapter/Serial_Adapter/Serial_Adapter.h`

Add includes before class declaration:
```cpp
#include "src/Mesh/serialization/nanopb/pb.h"
#include "src/Mesh/serialization/nanopb/pb_encode.h"
#include "src/Mesh/serialization/nanopb/pb_decode.h"
#include "src/Mesh/serialization/mesh.pb.h"
```

Remove old enrollment opcode constants (0xC0-0xC2 raw protocol replaced by Protobuf):
```cpp
// REMOVE:
static constexpr uint8_t OP_ENROLLMENT_REQ     = 0xC0;
static constexpr uint8_t OP_ENROLLMENT_APPROVE = 0xC1;
static constexpr uint8_t OP_ENROLLMENT_REJECT  = 0xC2;
```

Add join-ack message type alias:
```cpp
static constexpr uint32_t SERIAL_MSG_JOIN_ACK = 4;
// SERIAL_MSG_BROADCAST = 3 stays — it is the server→device command type
```

Remove all private hand-rolled codec method declarations:
```cpp
// REMOVE all of these:
size_t writeVarint(...);
size_t writeZigZag32(...);
size_t writeKey(...);
size_t writeBytesField(...);
size_t writeSint32Field(...);
size_t writeUint32Field(...);
bool readVarint(...);
bool readZigZag32(...);
bool readKey(...);
bool readLengthDelimited(...);
```

### `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp`

Remove all private helper implementations (everything from `writeVarint` through `readLengthDelimited`).

**Replace `encodeMeshMessage()`:**

```cpp
size_t Serial_Adapter::encodeMeshMessage(const planetopia::mesh::mesh_message& msg,
                                         uint8_t* out, size_t outCap) {
  MeshMessage pbMsg = MeshMessage_init_zero;
  pbMsg.messageType   = static_cast<uint32_t>(msg.messageType);
  pbMsg.dataType      = static_cast<int32_t>(msg.dataType);
  pbMsg.hopCount      = msg.hopCount;
  pbMsg.epochNum      = msg.epochNum;
  pbMsg.seqNum        = static_cast<uint32_t>(msg.seqNum);
  pbMsg.protoVersion  = static_cast<uint32_t>(msg.protoVersion);
  memcpy(pbMsg.originMacAddress,   msg.originMacAddress,   6);
  memcpy(pbMsg.targetMacAddress,   msg.targetMacAddress,   6);
  memcpy(pbMsg.lastHopMacAddress,  msg.lastHopMacAddress,  6);

  // data: set actual byte count (current protocol always uses 12)
  static_assert(sizeof(msg.data) == 12, "data field size mismatch");
  pbMsg.data_count = sizeof(msg.data);
  memcpy(pbMsg.data, msg.data, pbMsg.data_count);

  // public_key only for enrollment/join_ack frames with a real key
  if (msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_ENROLLMENT ||
      msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_JOIN_ACK) {
    bool nonZero = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollmentPublicKey[i]) { nonZero = true; break; }
    }
    if (nonZero) {
      pbMsg.has_public_key = true;
      memcpy(pbMsg.public_key, msg.enrollmentPublicKey, 32);
    }
  }

  pb_ostream_t stream = pb_ostream_from_buffer(out, outCap);
  if (!pb_encode(&stream, MeshMessage_fields, &pbMsg)) {
    return 0;
  }
  return stream.bytes_written;
}
```

**Replace `decodeMeshMessage()`:**

```cpp
bool Serial_Adapter::decodeMeshMessage(const uint8_t* data, size_t len,
                                       planetopia::mesh::mesh_message& outMsg) {
  memset(&outMsg, 0, sizeof(outMsg));

  MeshMessage pbMsg = MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, MeshMessage_fields, &pbMsg)) {
    return false;
  }

  outMsg.messageType  = static_cast<planetopia::mesh::MeshMessageType>(pbMsg.messageType);
  outMsg.dataType     = static_cast<planetopia::adapter::adapter_types>(pbMsg.dataType);
  outMsg.hopCount     = static_cast<uint8_t>(pbMsg.hopCount);
  outMsg.epochNum     = pbMsg.epochNum;
  outMsg.seqNum       = static_cast<uint16_t>(pbMsg.seqNum);
  outMsg.protoVersion = static_cast<uint8_t>(pbMsg.protoVersion);
  memcpy(outMsg.targetMacAddress, pbMsg.targetMacAddress, 6);
  size_t dataToCopy = pbMsg.data_count < 12 ? pbMsg.data_count : 12;
  memcpy(outMsg.data, pbMsg.data, dataToCopy);

  if (pbMsg.has_public_key) {
    memcpy(outMsg.enrollmentPublicKey, pbMsg.public_key, 32);
  }

  // Routing fields auto-filled — server only sends target; device fills origin/lastHop
  readOwnMac(outMsg.originMacAddress);
  readOwnMac(outMsg.lastHopMacAddress);

  return true;
}
```

**Replace `relayEnrollmentToServer()`:**

```cpp
void Serial_Adapter::relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]) {
  planetopia::mesh::mesh_message msg = {};
  msg.messageType  = planetopia::mesh::MeshMessageType::MESH_TYPE_ENROLLMENT;
  msg.protoVersion = 1;
  memcpy(msg.originMacAddress,    mac,    6);
  memcpy(msg.enrollmentPublicKey, pubKey, 32);

  uint8_t encoded[128];
  size_t n = encodeMeshMessage(msg, encoded, sizeof(encoded));
  if (n == 0) return;

  uint8_t lenLE[2] = {
    static_cast<uint8_t>(n & 0xFF),
    static_cast<uint8_t>((n >> 8) & 0xFF)
  };
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);
}
```

**Update `handleCompleteFrame()`:**

Remove the raw opcode detection block (was handling `0xC1` ENROLLMENT_APPROVE / `0xC2` REJECT in the raw binary format).

After `decodeMeshMessage()` succeeds, dispatch on `msg.messageType` before the existing ADAPTER_DATA/BROADCAST path:

```cpp
if (msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_JOIN_ACK) {
  if (pbMsg.has_public_key) {  // non-zero key = approved
    planetopia::mesh::Mesh* meshInstance = planetopia::mesh::Mesh::getInstance();
    if (meshInstance) {
      meshInstance->enrollPeer(msg.originMacAddress, msg.enrollmentPublicKey);
    }
  }
  // else: server rejected — no action needed (node retries on next enrollment interval)
  return;
}
```

Note: `pbMsg` is the decoded nanopb struct — pass it through or re-check `msg.enrollmentPublicKey` (which was filled from `pbMsg.public_key` if `has_public_key` was true).

### `tests/CMakeLists.txt`

In `FIRMWARE_SOURCES`:
- Remove: `../main/src/Mesh/serialization/ProtobufCodec.cpp`
- Add:
  ```cmake
  ../main/src/Mesh/serialization/mesh.pb.c
  ../main/src/Mesh/serialization/nanopb/pb_encode.c
  ../main/src/Mesh/serialization/nanopb/pb_decode.c
  ../main/src/Mesh/serialization/nanopb/pb_common.c
  ```

In `include_directories()`, add:
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/../main/src/Mesh/serialization/nanopb
${CMAKE_CURRENT_SOURCE_DIR}/../main/src/Mesh/serialization
```

Remove the test target:
```cmake
# Remove:
add_unit_test(test_protobuf_codec  unit/test_protobuf_codec.cpp)
```

### `tests/unit/test_serial_framing.cpp`

Replace with tests that verify nanopb encode→decode round-trip via `Serial_Adapter`'s public API:
- Encode a `mesh_message` (ADAPTER_DATA with 12-byte payload) → decode → fields match
- Encode ENROLLMENT with public key → decode → `has_public_key=true`, key bytes match
- Encode JOIN_ACK with zero key → decode → `has_public_key=false`
- Truncated buffer → `pb_decode` returns false
- Frame state machine tests (AwaitingLen1 → AwaitingLen2 → AwaitingPayload) remain unchanged

---

## Enrollment Wire Protocol Change

### Before (broken)
```
[2B LE length = 39][0xC0][6B mac][32B pubkey]   // raw binary, NOT Protobuf
```
Server's `proto.Unmarshal` tries to parse `0xC0...` → gets garbage.

### After (correct)
```
[2B LE length = N][Protobuf-encoded MeshMessage]
  messageType = 2 (ENROLLMENT)
  originMacAddress = enrolling node MAC (6 bytes)
  public_key = Curve25519 public key (32 bytes, has_public_key=true)
  protoVersion = 1
```
Server decodes cleanly → `msg.MessageType == 2` → enrollment handler.

### JOIN_ACK (server → master → node)

**Approved:**
```
messageType = 4 (JOIN_ACK)
originMacAddress = approved node MAC
public_key = 32-byte key (has_public_key=true)
```

**Rejected:**
```
messageType = 4 (JOIN_ACK)
originMacAddress = rejected node MAC
public_key = absent (has_public_key=false)
```

---

## Success Criteria

- `arduino-cli compile --fqbn esp32:esp32:esp32da main` — zero errors
- `cmake --build tests/build && ctest` — all tests green
- `test_serial_framing` exercises nanopb encode → decode round-trip (ADAPTER_DATA, ENROLLMENT, JOIN_ACK)
- Serial capture: enrollment frame decodes cleanly as valid Protobuf with `messageType=2` and `public_key` present
- JOIN_ACK with non-zero key triggers `enrollPeer()`; absent key is ignored
