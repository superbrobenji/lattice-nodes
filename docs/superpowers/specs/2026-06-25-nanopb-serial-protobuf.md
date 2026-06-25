# Firmware Spec: nanopb Serial Protobuf Migration

**Date:** 2026-06-25
**Repo:** planetopia-nodes (`feat/firmware-overhaul`)
**Related server change:** motionSensorServer commits f60d4fd..fa3dcf7 (field reorder + enrollment Protobuf)

---

## Goal

Replace the hand-rolled Protobuf encode/decode in `Serial_Adapter.cpp` (and the dead `ProtobufCodec.cpp`) with **nanopb** — the standard embedded Protobuf library for ESP32/Arduino. Fix the enrollment relay to send proper Protobuf frames instead of raw binary bytes, which currently breaks the server's `proto.Unmarshal` decoder.

No behaviour visible to the mesh (ESP-NOW layer) changes. Only the serial wire format for enrollment frames changes.

---

## Background / Why Now

The server was updated to match the agreed proto schema. Three bugs in the current firmware serial layer need fixing together:

1. **ProtobufCodec.cpp** — exists in `main/src/Mesh/serialization/` but is never used anywhere. Dead code compiled into every test binary.
2. **Serial_Adapter inline codec** — duplicates ProtobufCodec with its own hand-rolled varint/zigzag/bytes helpers. Two implementations of the same thing, neither with test coverage for edge cases.
3. **`relayEnrollmentToServer()`** — sends raw binary `[0xC0][6B mac][32B pubkey]` with a 2-byte length prefix. The server reads 2-byte length → tries `proto.Unmarshal(39 bytes starting with 0xC0)` → succeeds but gets a zero-value message → treats every enrollment request as a PIR event. **Enrollment is currently silently broken.**

---

## Proto Schema (source of truth)

Both firmware and server must use this schema. The server's `mesh.proto` was already updated to match this.

**`main/proto/mesh.proto`** (create this file):

```proto
syntax = "proto3";
package mesh;

message MeshMessage {
  uint32 messageType       = 1;  // 0=ADAPTER_DATA, 1=MASTER_BEACON, 2=ENROLLMENT, 3=JOIN_ACK
  sint32 dataType          = 2;  // zigzag: -1=UNKNOWN, 0=PIR, 1=WIFI, 2=LED, 3=SERIAL
  bytes  originMacAddress  = 3;  // 6 bytes
  bytes  targetMacAddress  = 4;  // 6 bytes
  bytes  lastHopMacAddress = 5;  // 6 bytes
  bytes  data              = 6;  // up to 12 bytes
  uint32 hopCount          = 7;
  uint32 epochNum          = 8;  // sender boot epoch
  uint32 seqNum            = 9;  // per-boot sequence counter
  uint32 protoVersion      = 10; // must be 1 for v1 messages
  bytes  public_key        = 11; // enrollment/join_ack: 32-byte Curve25519 public key
}
```

**`main/proto/mesh.options`** (nanopb field size annotations):

```
MeshMessage.originMacAddress  max_size:6  fixed_length:true
MeshMessage.targetMacAddress  max_size:6  fixed_length:true
MeshMessage.lastHopMacAddress max_size:6  fixed_length:true
MeshMessage.data              max_size:12
MeshMessage.public_key        max_size:32 fixed_length:true
```

The `fixed_length:true` fields generate `pb_byte_t field[N]` (direct array).
The `data` field without `fixed_length` generates `uint8_t data[12]; pb_size_t data_count;`.

---

## Toolchain Setup

nanopb pip package provides the generator only — the C runtime must be added separately.

### Generator (already installed)
```
python3 ...nanopb/generator/nanopb_generator.py
```

### C runtime
Copy these 7 files from nanopb 0.4.x release into `main/src/Mesh/serialization/nanopb/`:
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

Produces:
- `main/src/Mesh/serialization/mesh.pb.h`
- `main/src/Mesh/serialization/mesh.pb.c`

Both files are committed to the repo (generated from the committed proto).

---

## Files Changed

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
| `main/src/Mesh/Mesh.h` | Add `enrollmentPublicKey[32]` to `mesh_message` struct |
| `main/src/Adapter/Serial_Adapter/Serial_Adapter.h` | Remove private codec helpers + enrollment opcodes; add nanopb includes |
| `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp` | Replace encodeMeshMessage/decodeMeshMessage/relayEnrollmentToServer/handleCompleteFrame |
| `tests/CMakeLists.txt` | Swap ProtobufCodec.cpp for nanopb + mesh.pb.c; remove test_protobuf_codec target |
| `tests/unit/test_serial_framing.cpp` | Update to test nanopb encode/decode path |

---

## Detailed Changes

### `main/src/Mesh/Mesh.h` — mesh_message struct

Add one field at the end of the struct (zero-padded for normal messages):

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
  uint8_t enrollmentPublicKey[32]; // NEW: zero normally; set for ENROLLMENT/JOIN_ACK
};
```

This struct is the ESP-NOW (raw binary) internal representation. It is NOT the Protobuf schema. Size increases from 43 to 75 bytes — only affects in-memory representation, not the ESP-NOW payload (ESP-NOW still sends raw struct bytes).

> **Open question**: Is the ESP-NOW payload size constrained? ESP-NOW max payload is 250 bytes so 75 bytes is fine. But if any other code does `sizeof(mesh_message)` for a size assertion or allocation, check those sites.

### `main/src/Adapter/Serial_Adapter/Serial_Adapter.h`

Add before the class declaration:

```cpp
#include "src/Mesh/serialization/nanopb/pb.h"
#include "src/Mesh/serialization/nanopb/pb_encode.h"
#include "src/Mesh/serialization/nanopb/pb_decode.h"
#include "src/Mesh/serialization/mesh.pb.h"
```

Remove from the private section — all of these are replaced by nanopb:

```cpp
// REMOVE these private methods:
size_t writeVarint(uint8_t* out, uint32_t value);
size_t writeZigZag32(uint8_t* out, int32_t value);
size_t writeKey(uint8_t* out, uint32_t fieldNumber, uint8_t wireType);
size_t writeBytesField(uint8_t* out, uint32_t fieldNumber, const uint8_t* data, size_t len);
size_t writeSint32Field(uint8_t* out, uint32_t fieldNumber, int32_t value);
size_t writeUint32Field(uint8_t* out, uint32_t fieldNumber, uint32_t value);
bool readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out);
bool readZigZag32(const uint8_t*& ptr, const uint8_t* end, int32_t& out);
bool readKey(const uint8_t*& ptr, const uint8_t* end, uint32_t& fieldNumber, uint8_t& wireType);
bool readLengthDelimited(const uint8_t*& ptr, const uint8_t* end, const uint8_t*& dataPtr, size_t& dataLen);
```

Remove enrollment opcodes (raw binary protocol replaced by Protobuf):

```cpp
// REMOVE:
static constexpr uint8_t OP_ENROLLMENT_REQ     = 0xC0;
static constexpr uint8_t OP_ENROLLMENT_APPROVE = 0xC1;
static constexpr uint8_t OP_ENROLLMENT_REJECT  = 0xC2;
```

Keep all other opcodes (`OP_HEALTH_REQ`, `OP_HEALTH_REPORT`, `OP_CONFIG_SET`, `OP_TX_POWER_SET`).

### `main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp`

Remove all private helper implementations (writeVarint through readLengthDelimited).

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

  // data field: nanopb generates uint8_t data[12] + pb_size_t data_count
  pbMsg.data_count = 12;
  memcpy(pbMsg.data, msg.data, 12);

  // public_key only for enrollment/join_ack frames that carry a key
  if (msg.messageType == planetopia::mesh::MESH_TYPE_ENROLLMENT ||
      msg.messageType == planetopia::mesh::MESH_TYPE_JOIN_ACK) {
    bool nonZero = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollmentPublicKey[i]) { nonZero = true; break; }
    }
    if (nonZero) {
      memcpy(pbMsg.public_key, msg.enrollmentPublicKey, 32);
      // proto3 bytes with fixed_length — nanopb always encodes if max_size is set
      // No has_ field needed; if all-zero, the bytes field is omitted by pb_encode
    }
  }

  pb_ostream_t stream = pb_ostream_from_buffer(out, outCap);
  if (!pb_encode(&stream, MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter",
      String("pb_encode failed: ") + PB_GET_ERROR(&stream), LogLevel::LOG_ERROR);
    return 0;
  }
  return stream.bytes_written;
}
```

> **Note on `data_count`**: nanopb generates `data_count` for `bytes` fields with `max_size` but without `fixed_length`. `data_count` is the actual byte count to encode. Always set it to the real payload length, not always 12. The current protocol always uses 12 bytes — if that changes, update accordingly.

> **Note on `public_key`**: With `fixed_length:true`, nanopb generates `pb_byte_t public_key[32]` with no `has_` field. If the key bytes are all zero, pb_encode will still encode the field (fixed_length fields are always included). The server will receive 32 zero bytes and interpret it as a rejection. This is the intended behavior for JOIN_ACK with no key.
>
> **Alternative**: if you want to suppress the key field entirely for rejections, use an `.options` entry of `max_size:32` without `fixed_length:true` — that generates `has_public_key` + `public_key[32]`, and you set `has_public_key = nonZero`.

**Replace `decodeMeshMessage()`:**

```cpp
bool Serial_Adapter::decodeMeshMessage(const uint8_t* data, size_t len,
                                       planetopia::mesh::mesh_message& outMsg) {
  memset(&outMsg, 0, sizeof(outMsg));

  MeshMessage pbMsg = MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter",
      String("pb_decode failed: ") + PB_GET_ERROR(&stream), LogLevel::LOG_ERROR);
    return false;
  }

  outMsg.messageType  = static_cast<planetopia::mesh::MeshMessageType>(pbMsg.messageType);
  outMsg.dataType     = static_cast<planetopia::adapter::adapter_types>(pbMsg.dataType);
  outMsg.hopCount     = static_cast<uint8_t>(pbMsg.hopCount);
  outMsg.epochNum     = pbMsg.epochNum;
  outMsg.seqNum       = static_cast<uint16_t>(pbMsg.seqNum);
  outMsg.protoVersion = static_cast<uint8_t>(pbMsg.protoVersion);
  memcpy(outMsg.targetMacAddress, pbMsg.targetMacAddress, 6);
  memcpy(outMsg.data, pbMsg.data, pbMsg.data_count < 12 ? pbMsg.data_count : 12);
  memcpy(outMsg.enrollmentPublicKey, pbMsg.public_key, 32);

  // Auto-generate routing fields (server only sends essential fields)
  readOwnMac(outMsg.originMacAddress);
  readOwnMac(outMsg.lastHopMacAddress);

  return true;
}
```

**Replace `relayEnrollmentToServer()`:**

```cpp
void Serial_Adapter::relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]) {
  planetopia::mesh::mesh_message msg = {};
  msg.messageType = planetopia::mesh::MESH_TYPE_ENROLLMENT;
  msg.protoVersion = 1;
  memcpy(msg.originMacAddress, mac, 6);
  memcpy(msg.enrollmentPublicKey, pubKey, 32);

  uint8_t encoded[128];
  size_t n = encodeMeshMessage(msg, encoded, sizeof(encoded));
  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode enrollment request", LogLevel::LOG_ERROR);
    return;
  }
  uint8_t lenLE[2] = {
    static_cast<uint8_t>(n & 0xFF),
    static_cast<uint8_t>((n >> 8) & 0xFF)
  };
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);
  Logger::logln("Serial_Adapter", "Enrollment request sent as Protobuf", LogLevel::LOG_INFO);
}
```

**Update `handleCompleteFrame()`:**

Remove the raw opcode detection block at the top:
```cpp
// REMOVE this entire block:
if (len >= 1) {
    uint8_t op = data[0];
    if (op == OP_ENROLLMENT_APPROVE && len >= 39) { ... }
    else if (op == OP_ENROLLMENT_REJECT) { ... }
}
```

After `decodeMeshMessage()` succeeds, add JOIN_ACK handling before the existing ADAPTER_DATA/BROADCAST handling:

```cpp
if (msg.messageType == planetopia::mesh::MESH_TYPE_JOIN_ACK) {
  bool approved = false;
  for (int i = 0; i < 32; ++i) {
    if (msg.enrollmentPublicKey[i]) { approved = true; break; }
  }
  if (approved) {
    Logger::logln("Serial_Adapter", "Server approved enrollment, registering peer", LogLevel::LOG_INFO);
    planetopia::mesh::Mesh* meshInstance = planetopia::mesh::Mesh::getInstance();
    if (meshInstance) {
      meshInstance->enrollPeer(msg.originMacAddress, msg.enrollmentPublicKey);
    }
  } else {
    Logger::logln("Serial_Adapter", "Server rejected enrollment request", LogLevel::LOG_WARN);
  }
  return;
}
```

### `tests/CMakeLists.txt`

In `FIRMWARE_SOURCES`:
- **Remove**: `../main/src/Mesh/serialization/ProtobufCodec.cpp`
- **Add**:
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
add_unit_test(test_protobuf_codec     unit/test_protobuf_codec.cpp)
```

### `tests/unit/test_serial_framing.cpp`

Update to test nanopb encode/decode instead of ProtobufCodec. The test intent stays the same: encode a `mesh_message` → decode it → fields match. Remove any direct ProtobufCodec includes or calls.

---

## Enrollment Wire Protocol Change

### Before (broken)
```
[2B LE length = 39][0xC0][6B mac][32B pubkey]   // raw binary, NOT Protobuf
```
Server's `proto.Unmarshal` tries to parse `0xC0...` as Protobuf → gets garbage → treats as PIR event.

### After (correct)
```
[2B LE length = N][Protobuf-encoded MeshMessage]
  messageType = 2 (ENROLLMENT)
  originMacAddress = enrolling node MAC (6 bytes)
  public_key = Curve25519 public key (32 bytes)
  protoVersion = 1
```
Server decodes cleanly via `proto.Unmarshal` → `msg.MessageType == 2` → enrollment handler.

### Approval (server → master → node)
```
[2B LE length = N][Protobuf-encoded MeshMessage]
  messageType = 3 (JOIN_ACK)
  originMacAddress = approved node MAC (6 bytes)
  public_key = approved Curve25519 public key (32 bytes, non-zero = approved)
```

### Rejection (server → master → node)
```
[2B LE length = N][Protobuf-encoded MeshMessage]
  messageType = 3 (JOIN_ACK)
  originMacAddress = rejected node MAC (6 bytes)
  public_key absent / all-zero = rejection
```

---

## Open Questions for Review

1. **`enrollmentPublicKey` in `mesh_message` struct**: Adding 32 bytes increases the struct from 43 to 75 bytes. All ESP-NOW code sends `sizeof(mesh_message)` bytes — this increases the ESP-NOW payload size. Confirm no ESP-NOW peer has a hardcoded payload size check. (ESP-NOW limit is 250 bytes so capacity is fine.)

2. **`fixed_length:true` on `public_key`**: With `fixed_length:true`, pb_encode always sends 32 zero bytes even for non-enrollment messages where `enrollmentPublicKey` is zero-filled. This adds ~34 bytes (field tag + length + 32 bytes) to every message. Alternative: remove `fixed_length:true` from `public_key`, use `has_public_key` bool, only set it when there's a real key. This saves ~34 bytes per non-enrollment message.

3. **`data` field always sent as 12 bytes**: Currently `encodeMeshMessage` always writes 12 bytes for `data`. If the actual payload is shorter (e.g., a health report uses 12 bytes but a PIR report might use fewer), `data_count` should reflect the real length. Existing protocol always packs 12 bytes, so this is a no-op change — but it's worth confirming.

4. **Proto version on all outgoing frames**: `relayEnrollmentToServer` sets `protoVersion = 1`. Other outgoing serial frames (health reports sent as ADAPTER_DATA) set `protoVersion` via `buildMessage()`. Confirm all outgoing serial frames set `protoVersion = 1`.

---

## Success Criteria

- `cmake --build tests/build` passes with zero errors and all tests green
- `test_serial_framing` exercises nanopb encode → decode round-trip
- Serial output: enrollment frame parses as valid Protobuf on the server (`proto.Unmarshal` succeeds, `messageType == 2`)
- JOIN_ACK from server decoded correctly: non-zero `public_key` → `enrollPeer` called; zero/absent key → rejection logged
