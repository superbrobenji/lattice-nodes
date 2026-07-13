# Message Route Tracking & Protocol Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `MESH_TYPE_ROUTE_REPORT` to the mesh protocol — each non-master node sends its full MAC hop chain to the server every 60 seconds — and migrate `MeshMessageType`, `mesh_message`, and `mesh.proto` into `lattice-protocol` as Go-driven codegen.

**Architecture:** lattice-protocol gains a `message/` package whose Go structs (with `c` and `proto` struct tags) are read by an extended `cmd/gen-headers` to emit `c/message_types.h`, `c/mesh_message.h`, and `proto/mesh.proto`. lattice-nodes then removes its inline enum/struct from `Mesh.h`, includes the generated headers, migrates the proto source, and adds `sendRouteReport()` + `processRouteReport()` to the Mesh class.

**Tech Stack:** Go 1.21, reflect package (codegen), C++17 (firmware), GoogleTest 1.14 (tests), nanopb 0.4.9 (proto→C serialization), PlatformIO (firmware flash build).

## Global Constraints

- `lattice-protocol` module path: `github.com/superbrobenji/lattice-protocol`
- Wire struct size must remain 127 bytes — `static_assert` enforces this
- `MESH_TYPE_ROUTE_REPORT = uint8(5)` — do not renumber existing types
- `OpRouteReport = byte(0xB3)` — do not reassign existing opcodes
- Route report payload: `data[0]=0xB3, data[1]=path_len(0..10), data[2..61]=relay MACs` — fits in `data[64]` with 2 bytes spare
- All tests: `cd tests && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure`
- Go tests: `cd lattice-protocol && go test ./...`
- Codegen verify: `cd lattice-protocol && make check`
- lattice-protocol repo root: `../lattice-protocol` relative to lattice-nodes
- lattice-nodes submodule path: `main/lib/lattice-protocol`

---

## File Map

### lattice-protocol (create/modify)

| Action | Path | Responsibility |
|---|---|---|
| Create | `message/types.go` | `MeshMessageType` uint8 constants |
| Create | `message/message.go` | `MeshMessage` struct with `c`/`proto` tags |
| Modify | `cmd/gen-headers/main.go` | extend to emit `c/message_types.h`, `c/mesh_message.h`, `proto/mesh.proto` |
| Modify | `opcodes/opcodes.go` | add `OpRouteReport = byte(0xB3)` |
| Modify | `Makefile` | extend `check` to also diff `proto/` |
| Generated | `c/message_types.h` | DO NOT EDIT — from `message/types.go` |
| Generated | `c/mesh_message.h` | DO NOT EDIT — from `message/message.go` |
| Generated | `proto/mesh.proto` | DO NOT EDIT — from `message/message.go` |
| Generated | `c/opcodes.h` | DO NOT EDIT — from `opcodes/opcodes.go` |

### lattice-nodes (create/modify/delete)

| Action | Path | Responsibility |
|---|---|---|
| Modify | `main/src/Mesh/Mesh.h` | remove inline enum/struct; add includes; add `lastRouteReportMs`, `sendRouteReport()`, `processRouteReport()` declarations |
| Modify | `main/project_config.h` | add `ROUTE_REPORT_INTERVAL_MS` |
| Modify | `main/src/Mesh/Mesh.cpp` | add timer call to `loop()` |
| Modify | `tests/mocks/mesh_logic_impl.cpp` | add `sendRouteReport()`, `processRouteReport()`, update `drainRecvQueue()` switch |
| Delete | `main/proto/mesh.proto` | replaced by generated file in submodule |
| Move | `main/proto/mesh.options` → `main/lib/lattice-protocol/proto/mesh.options` | nanopb options travel with proto |
| Regenerate | `main/src/Mesh/serialization/mesh.pb.h` | from new proto path |
| Regenerate | `main/src/Mesh/serialization/mesh.pb.c` | from new proto path |
| Create | `tests/unit/test_route_report.cpp` | GoogleTest: `sendRouteReport` + `processRouteReport` |
| Modify | `tests/CMakeLists.txt` | register `test_route_report` |

---

## Task 1: lattice-protocol — message type constants + C header

**Files:**
- Create: `message/types.go`
- Modify: `cmd/gen-headers/main.go`
- Generated: `c/message_types.h`

**Interfaces:**
- Produces: `MeshTypeAdapterData`, `MeshTypeMasterBeacon`, `MeshTypeEnrollment`, `MeshTypeSerialCmdBroadcast`, `MeshTypeJoinAck`, `MeshTypeRouteReport` as `uint8` Go constants; `MESH_TYPE_*` C `#define` macros; `MeshMessageType` C typedef

- [ ] **Step 1: Create `message/types.go`**

```go
// Package message defines the Lattice mesh message type constants.
// C headers in c/ are generated from these constants — run "go generate ./..." to regenerate.
//
//go:generate go run ../cmd/gen-headers/main.go
package message

const (
	MeshTypeAdapterData        = uint8(0) // node data relayed toward server
	MeshTypeMasterBeacon       = uint8(1) // master→mesh: topology beacon
	MeshTypeEnrollment         = uint8(2) // node→master: enrollment request
	MeshTypeSerialCmdBroadcast = uint8(3) // server→node: serial command broadcast
	MeshTypeJoinAck            = uint8(4) // server→node: enrollment approved
	MeshTypeRouteReport        = uint8(5) // node→server: routing path report
)
```

Save to `../lattice-protocol/message/types.go`.

- [ ] **Step 2: Add `writeMeshMessageTypesHeader` to gen-headers**

Open `../lattice-protocol/cmd/gen-headers/main.go`. Add the import and the new function, then call it from `main()`:

```go
import (
    "fmt"
    "os"
    "path/filepath"

    "github.com/superbrobenji/lattice-protocol/adapter"
    "github.com/superbrobenji/lattice-protocol/message"
    "github.com/superbrobenji/lattice-protocol/opcodes"
)

func main() {
    root := repoRoot()
    must(writeOpcodesHeader(filepath.Join(root, "c", "opcodes.h")))
    must(writeAdapterTypesHeader(filepath.Join(root, "c", "adapter_types.h")))
    must(writeMeshMessageTypesHeader(filepath.Join(root, "c", "message_types.h")))
}
```

Add the function body after `writeAdapterTypesHeader`:

```go
func writeMeshMessageTypesHeader(path string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	if _, err := fmt.Fprint(f, `// Code generated by cmd/gen-headers; DO NOT EDIT.
// Source of truth: message/types.go — run "go generate ./..." to regenerate.

#pragma once

/* Lattice mesh message type identifiers.
 * Carried in the messageType field of every mesh_message frame. */

typedef uint8_t MeshMessageType;

`); err != nil {
		_ = f.Close()
		return err
	}
	writeCDefUint8("MESH_TYPE_ADAPTER_DATA", message.MeshTypeAdapterData, "node data relayed toward server", f)
	writeCDefUint8("MESH_TYPE_MASTER_BEACON", message.MeshTypeMasterBeacon, "master→mesh: topology beacon", f)
	writeCDefUint8("MESH_TYPE_ENROLLMENT", message.MeshTypeEnrollment, "node→master: enrollment request", f)
	writeCDefUint8("MESH_TYPE_SERIAL_CMD_BROADCAST", message.MeshTypeSerialCmdBroadcast, "server→node: serial command broadcast", f)
	writeCDefUint8("MESH_TYPE_JOIN_ACK", message.MeshTypeJoinAck, "server→node: enrollment approved", f)
	writeCDefUint8("MESH_TYPE_ROUTE_REPORT", message.MeshTypeRouteReport, "node→server: routing path report", f)
	return f.Close()
}

func writeCDefUint8(name string, val uint8, comment string, f *os.File) {
	_, err := fmt.Fprintf(f, "#define %-32s ((MeshMessageType)%d)  /* %s */\n", name, val, comment)
	must(err)
}
```

- [ ] **Step 3: Run codegen**

```bash
cd ../lattice-protocol && go generate ./...
```

Expected: `c/message_types.h` created with 6 `#define` lines and `typedef uint8_t MeshMessageType;`.

- [ ] **Step 4: Verify generated header**

```bash
cat ../lattice-protocol/c/message_types.h
```

Expected output:
```c
// Code generated by cmd/gen-headers; DO NOT EDIT.
// Source of truth: message/types.go — run "go generate ./..." to regenerate.

#pragma once

/* Lattice mesh message type identifiers.
 * Carried in the messageType field of every mesh_message frame. */

typedef uint8_t MeshMessageType;

#define MESH_TYPE_ADAPTER_DATA           ((MeshMessageType)0)  /* node data relayed toward server */
#define MESH_TYPE_MASTER_BEACON          ((MeshMessageType)1)  /* master→mesh: topology beacon */
#define MESH_TYPE_ENROLLMENT             ((MeshMessageType)2)  /* node→master: enrollment request */
#define MESH_TYPE_SERIAL_CMD_BROADCAST   ((MeshMessageType)3)  /* server→node: serial command broadcast */
#define MESH_TYPE_JOIN_ACK               ((MeshMessageType)4)  /* server→node: enrollment approved */
#define MESH_TYPE_ROUTE_REPORT           ((MeshMessageType)5)  /* node→server: routing path report */
```

- [ ] **Step 5: Run Go tests**

```bash
cd ../lattice-protocol && go test ./...
```

Expected: `ok github.com/superbrobenji/lattice-protocol/...` for all packages.

- [ ] **Step 6: Commit**

```bash
cd ../lattice-protocol
git add message/types.go cmd/gen-headers/main.go c/message_types.h
git commit -m "feat: add MeshMessageType constants and generate c/message_types.h"
```

---

## Task 2: lattice-protocol — message struct + C header generation

**Files:**
- Create: `message/message.go`
- Modify: `cmd/gen-headers/main.go`
- Generated: `c/mesh_message.h`

**Interfaces:**
- Consumes: `message/types.go` (Task 1), `adapter.TypeUnknown` etc.
- Produces: `c/mesh_message.h` with `mesh_message` packed C struct, `static_assert(sizeof(mesh_message) == 127, ...)`

- [ ] **Step 1: Create `message/message.go`**

```go
// Package message defines the Lattice mesh wire-format message struct.
// Struct tags drive codegen: `c` → packed C struct field type, `proto` → "fieldNum,protoType[,optional][,protoName]".
// Run "go generate ./..." to regenerate c/mesh_message.h and proto/mesh.proto.
package message

// MeshMessage is the 127-byte packed wire-format frame for the Lattice mesh.
// Field order matches the packed C struct — do not reorder without updating the static_assert.
type MeshMessage struct {
	ProtoVersion        uint8    `c:"uint8_t"     proto:"10,uint32"`
	MessageType         uint8    `c:"uint8_t"     proto:"1,uint32"`
	DataType            int32    `c:"int32_t"     proto:"2,sint32"`
	OriginMacAddress    [6]byte  `c:"uint8_t[6]"  proto:"3,bytes"`
	TargetMacAddress    [6]byte  `c:"uint8_t[6]"  proto:"4,bytes"`
	LastHopMacAddress   [6]byte  `c:"uint8_t[6]"  proto:"5,bytes"`
	Data                [64]byte `c:"uint8_t[64]" proto:"6,bytes,optional"`
	HopCount            uint8    `c:"uint8_t"     proto:"7,uint32"`
	EpochNum            uint32   `c:"uint32_t"    proto:"8,uint32"`
	SeqNum              uint16   `c:"uint16_t"    proto:"9,uint32"`
	EnrollmentPublicKey [32]byte `c:"uint8_t[32]" proto:"11,bytes,optional,public_key"`
}

// WireSize is the expected packed byte size — enforced by static_assert in the generated C header.
const WireSize = 127
```

Save to `../lattice-protocol/message/message.go`.

- [ ] **Step 2: Add `writeMeshMessageHeader` to gen-headers**

Add the following imports and function to `cmd/gen-headers/main.go`. Add the call in `main()` after the existing calls:

```go
import (
    "fmt"
    "os"
    "path/filepath"
    "reflect"
    "strings"
    "unicode"

    "github.com/superbrobenji/lattice-protocol/adapter"
    "github.com/superbrobenji/lattice-protocol/message"
    "github.com/superbrobenji/lattice-protocol/opcodes"
)

func main() {
    root := repoRoot()
    must(writeOpcodesHeader(filepath.Join(root, "c", "opcodes.h")))
    must(writeAdapterTypesHeader(filepath.Join(root, "c", "adapter_types.h")))
    must(writeMeshMessageTypesHeader(filepath.Join(root, "c", "message_types.h")))
    must(writeMeshMessageHeader(filepath.Join(root, "c", "mesh_message.h")))
}
```

Add the function:

```go
func writeMeshMessageHeader(path string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()

	if _, err := fmt.Fprint(f, `// Code generated by cmd/gen-headers; DO NOT EDIT.
// Source of truth: message/message.go — run "go generate ./..." to regenerate.

#pragma once
#include <stdint.h>
#include <assert.h>
#include "adapter_types.h"
#include "message_types.h"

`); err != nil {
		return err
	}

	if _, err := fmt.Fprint(f, "typedef struct __attribute__((packed)) {\n"); err != nil {
		return err
	}

	t := reflect.TypeOf(message.MeshMessage{})
	for i := 0; i < t.NumField(); i++ {
		field := t.Field(i)
		cType := field.Tag.Get("c")
		cName := pascalToSnake(field.Name)
		// Array types: "uint8_t[6]" → "uint8_t name[6]"
		if idx := strings.Index(cType, "["); idx >= 0 {
			baseType := cType[:idx]
			arraySuffix := cType[idx:]
			if _, err := fmt.Fprintf(f, "    %-16s %s%s;\n", baseType, cName, arraySuffix); err != nil {
				return err
			}
		} else {
			if _, err := fmt.Fprintf(f, "    %-16s %s;\n", cType, cName); err != nil {
				return err
			}
		}
	}

	if _, err := fmt.Fprintf(f, "} mesh_message;\n\nstatic_assert(sizeof(mesh_message) == %d, \"mesh_message size changed — update server proto\");\n", message.WireSize); err != nil {
		return err
	}
	return nil
}

// pascalToSnake converts PascalCase to snake_case. "OriginMacAddress" → "origin_mac_address".
func pascalToSnake(s string) string {
	var out []rune
	for i, r := range s {
		if unicode.IsUpper(r) && i > 0 {
			out = append(out, '_')
		}
		out = append(out, unicode.ToLower(r))
	}
	return string(out)
}

// protoFieldName returns the proto3 field name for a Go field.
// Uses the 4th element of the proto tag if present, otherwise lowerCamelCase of Go name.
func protoFieldName(goName, protoTag string) string {
	parts := strings.Split(protoTag, ",")
	if len(parts) >= 4 && parts[3] != "" {
		return parts[3]
	}
	// lowerCamelCase: "OriginMacAddress" → "originMacAddress"
	runes := []rune(goName)
	runes[0] = unicode.ToLower(runes[0])
	return string(runes)
}
```

- [ ] **Step 3: Run codegen and verify**

```bash
cd ../lattice-protocol && go generate ./...
cat ../lattice-protocol/c/mesh_message.h
```

Expected: `typedef struct __attribute__((packed)) {` followed by fields in declaration order, ending with `static_assert(sizeof(mesh_message) == 127, ...)`.

Verify field names match the existing `Mesh.h` struct (they must be identical for binary compatibility):

| C field name | Expected |
|---|---|
| `proto_version` | `uint8_t` |
| `message_type` | `uint8_t` |
| `data_type` | `int32_t` |
| `origin_mac_address` | `uint8_t[6]` |
| `target_mac_address` | `uint8_t[6]` |
| `last_hop_mac_address` | `uint8_t[6]` |
| `data` | `uint8_t[64]` |
| `hop_count` | `uint8_t` |
| `epoch_num` | `uint32_t` |
| `seq_num` | `uint16_t` |
| `enrollment_public_key` | `uint8_t[32]` |

> **Note:** The existing `Mesh.h` struct uses camelCase field names (`protoVersion`, `hopCount`, etc.). The generated header uses snake_case. Task 4 updates all references in `Mesh.h`, `Mesh.cpp`, and `mesh_logic_impl.cpp` to use the new snake_case names.

- [ ] **Step 4: Run Go tests**

```bash
cd ../lattice-protocol && go test ./...
```

Expected: all packages pass.

- [ ] **Step 5: Commit**

```bash
cd ../lattice-protocol
git add message/message.go cmd/gen-headers/main.go c/mesh_message.h
git commit -m "feat: add MeshMessage struct and generate c/mesh_message.h"
```

---

## Task 3: lattice-protocol — proto generation + OpRouteReport opcode

**Files:**
- Modify: `cmd/gen-headers/main.go`
- Modify: `opcodes/opcodes.go`
- Modify: `Makefile`
- Generated: `proto/mesh.proto`, `c/opcodes.h`

**Interfaces:**
- Consumes: `message/message.go` (Task 2)
- Produces: `proto/mesh.proto` matching existing nanopb-compatible field tags; `OP_ROUTE_REPORT 0xB3` in `c/opcodes.h`

- [ ] **Step 1: Create `proto/` directory and add proto generator to gen-headers**

```bash
mkdir -p ../lattice-protocol/proto
```

Add to `cmd/gen-headers/main.go`:

In `main()`, add after the existing calls:
```go
must(writeMeshProto(filepath.Join(root, "proto", "mesh.proto")))
```

Add the function:

```go
func writeMeshProto(path string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()

	if _, err := fmt.Fprint(f, `// Code generated by cmd/gen-headers; DO NOT EDIT.
// Source of truth: message/message.go — run "go generate ./..." to regenerate.
syntax = "proto3";
package mesh;

message MeshMessage {
`); err != nil {
		return err
	}

	t := reflect.TypeOf(message.MeshMessage{})
	// collect fields sorted by proto field number
	type protoField struct {
		num      int
		protoType string
		optional bool
		name     string
	}
	var fields []protoField
	for i := 0; i < t.NumField(); i++ {
		field := t.Field(i)
		tag := field.Tag.Get("proto")
		if tag == "" {
			continue
		}
		parts := strings.Split(tag, ",")
		num := 0
		fmt.Sscanf(parts[0], "%d", &num)
		protoType := parts[1]
		optional := false
		for _, p := range parts[2:] {
			if p == "optional" {
				optional = true
			}
		}
		name := protoFieldName(field.Name, tag)
		fields = append(fields, protoField{num, protoType, optional, name})
	}
	// sort by field number
	for i := 0; i < len(fields); i++ {
		for j := i + 1; j < len(fields); j++ {
			if fields[j].num < fields[i].num {
				fields[i], fields[j] = fields[j], fields[i]
			}
		}
	}
	for _, pf := range fields {
		optStr := ""
		if pf.optional {
			optStr = "optional "
		}
		if _, err := fmt.Fprintf(f, "  %s%s %s = %d;\n", optStr, pf.protoType, pf.name, pf.num); err != nil {
			return err
		}
	}
	_, err = fmt.Fprint(f, "}\n")
	return err
}
```

- [ ] **Step 2: Add `OpRouteReport` to `opcodes/opcodes.go`**

Open `../lattice-protocol/opcodes/opcodes.go`. Add after `OpNodeHealth`:

```go
OpRouteReport  = byte(0xB3) // Node→server via serial: routing path; payload: [B3][1B path_len][path_len × 6B MACs]
```

Also add a comment line above it:
```go
// Node → server: route reporting
OpRouteReport  = byte(0xB3) // Node→server via serial: routing path; payload: [B3][1B path_len][path_len × 6B MACs]
```

The full health reporting block becomes:
```go
// Health reporting — bidirectional between server and nodes.
OpHealthReq    = byte(0xB0) // Server → node: request health report; payload: [B0] (no body)
OpHealthReport = byte(0xB1) // Node (serial) → server: health status; payload: [B1][1B adapterType][6B mac][4B uptimeSec LE]
OpNodeHealth   = byte(0xB2) // Node (non-serial) → server via serial adapter; payload: [B2][1B adapterType][6B mac][4B uptimeSec LE]

// Node → server: route reporting
OpRouteReport  = byte(0xB3) // Node→server: routing path; payload: [B3][1B path_len][path_len × 6B MACs]
```

- [ ] **Step 3: Add `OP_ROUTE_REPORT` emission in `writeOpcodesHeader`**

In `cmd/gen-headers/main.go`, inside `writeOpcodesHeader`, add after the `OP_NODE_HEALTH` line:

```go
if _, err := fmt.Fprint(f, "\n/* Node → server: route reporting */\n"); err != nil {
    _ = f.Close()
    return err
}
writeCDefByte(f, "OP_ROUTE_REPORT", opcodes.OpRouteReport, "routing path: [B3][1B path_len][path_len × 6B MACs]")
```

- [ ] **Step 4: Update `Makefile` to verify `proto/`**

Open `../lattice-protocol/Makefile`. Change `check` target:

```makefile
check: generate
	git diff --exit-code c/ proto/
```

- [ ] **Step 5: Run codegen**

```bash
cd ../lattice-protocol && go generate ./...
```

- [ ] **Step 6: Verify `proto/mesh.proto`**

```bash
cat ../lattice-protocol/proto/mesh.proto
```

Expected (fields sorted by number):
```proto
// Code generated by cmd/gen-headers; DO NOT EDIT.
// Source of truth: message/message.go — run "go generate ./..." to regenerate.
syntax = "proto3";
package mesh;

message MeshMessage {
  uint32 messageType = 1;
  sint32 dataType = 2;
  bytes originMacAddress = 3;
  bytes targetMacAddress = 4;
  bytes lastHopMacAddress = 5;
  optional bytes data = 6;
  uint32 hopCount = 7;
  uint32 epochNum = 8;
  uint32 seqNum = 9;
  uint32 protoVersion = 10;
  optional bytes public_key = 11;
}
```

- [ ] **Step 7: Verify `c/opcodes.h` contains `OP_ROUTE_REPORT`**

```bash
grep ROUTE_REPORT ../lattice-protocol/c/opcodes.h
```

Expected: `#define OP_ROUTE_REPORT              0xB3  /* routing path: ... */`

- [ ] **Step 8: Run `make check`**

```bash
cd ../lattice-protocol && make check
```

Expected: exits 0 (no git diff in `c/` or `proto/`).

- [ ] **Step 9: Run Go tests**

```bash
cd ../lattice-protocol && go test ./...
```

Expected: all packages pass.

- [ ] **Step 10: Commit**

```bash
cd ../lattice-protocol
git add opcodes/opcodes.go cmd/gen-headers/main.go Makefile c/opcodes.h proto/mesh.proto
git commit -m "feat: generate proto/mesh.proto and add OpRouteReport (0xB3)"
```

---

## Task 4: lattice-nodes — migrate Mesh.h to generated headers

**Files:**
- Modify: `main/src/Mesh/Mesh.h`
- Modify: `main/src/Mesh/Mesh.cpp` (field name updates)
- Modify: `tests/mocks/mesh_logic_impl.cpp` (field name updates)

**Interfaces:**
- Consumes: `lib/lattice-protocol/c/message_types.h` (Task 1), `lib/lattice-protocol/c/mesh_message.h` (Task 2)
- Produces: `Mesh.h` free of inline enum/struct; all field accesses updated to snake_case

> First, sync the submodule to pick up the lattice-protocol changes from Tasks 1–3:
> ```bash
> cd main/lib/lattice-protocol && git pull origin main && cd ../../..
> git add main/lib/lattice-protocol && git commit -m "chore: update lattice-protocol submodule"
> ```

- [ ] **Step 1: Replace inline enum and struct in `Mesh.h`**

In `main/src/Mesh/Mesh.h`, find and remove:

```cpp
// --- Mesh protocol message type ---
enum MeshMessageType : uint8_t {
  MESH_TYPE_ADAPTER_DATA = 0,
  MESH_TYPE_MASTER_BEACON = 1,
  MESH_TYPE_ENROLLMENT = 2,
  MESH_TYPE_SERIAL_CMD_BROADCAST = 3, // server→device only
  MESH_TYPE_JOIN_ACK =
      4, // server→device only; was 3, changed to avoid collision with SERIAL_CMD_BROADCAST
};

static constexpr uint8_t PROTO_VERSION = 2;

// --- Mesh message struct (packed: wire protocol, no padding) ---
struct __attribute__((packed)) mesh_message {
  uint8_t protoVersion; // Always PROTO_VERSION (2)
  MeshMessageType messageType;
  adapter_types dataType;
  uint8_t originMacAddress[6];
  uint8_t targetMacAddress[6];
  uint8_t lastHopMacAddress[6];
  uint8_t data[64];
  uint8_t hopCount;
  uint32_t epochNum;               // Boot count of origin node (replay protection)
  uint16_t seqNum;                 // Per-boot message counter (replay protection)
  uint8_t enrollmentPublicKey[32]; // Curve25519 key; zero for non-enrollment messages
};
// 1+1+4+6+6+6+64+1+4+2+32 = 127 bytes (adapter_types is int32_t = 4B, packed)
static_assert(sizeof(mesh_message) == 127, "mesh_message size changed — update server proto");
```

Replace with:

```cpp
#include "../../lib/lattice-protocol/c/message_types.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

static constexpr uint8_t PROTO_VERSION = 2;
```

- [ ] **Step 2: Update all camelCase field references to snake_case**

The generated struct uses snake_case. Find every field access in `Mesh.cpp`, `mesh_logic_impl.cpp`, and `Mesh.h` and rename:

| Old name | New name |
|---|---|
| `msg.protoVersion` | `msg.proto_version` |
| `msg.messageType` | `msg.message_type` |
| `msg.dataType` | `msg.data_type` |
| `msg.originMacAddress` | `msg.origin_mac_address` |
| `msg.targetMacAddress` | `msg.target_mac_address` |
| `msg.lastHopMacAddress` | `msg.last_hop_mac_address` |
| `msg.hopCount` | `msg.hop_count` |
| `msg.epochNum` | `msg.epoch_num` |
| `msg.seqNum` | `msg.seq_num` |
| `msg.enrollmentPublicKey` | `msg.enrollment_public_key` |

Run a mechanical rename across the two files:

```bash
sed -i '' \
  -e 's/\.protoVersion/.proto_version/g' \
  -e 's/\.messageType/.message_type/g' \
  -e 's/\.dataType/.data_type/g' \
  -e 's/\.originMacAddress/.origin_mac_address/g' \
  -e 's/\.targetMacAddress/.target_mac_address/g' \
  -e 's/\.lastHopMacAddress/.last_hop_mac_address/g' \
  -e 's/\.hopCount/.hop_count/g' \
  -e 's/\.epochNum/.epoch_num/g' \
  -e 's/\.seqNum/.seq_num/g' \
  -e 's/\.enrollmentPublicKey/.enrollment_public_key/g' \
  main/src/Mesh/Mesh.cpp \
  tests/mocks/mesh_logic_impl.cpp
```

Also update the `buildMessage`, `printMeshMessage` method bodies in `Mesh.cpp` and any test helpers in `test_mesh_logic.cpp`, `test_replay_cache.cpp`, `test_serial_framing.cpp` that construct `mesh_message{}` structs.

Run the same sed across test files:

```bash
sed -i '' \
  -e 's/\.protoVersion/.proto_version/g' \
  -e 's/\.messageType/.message_type/g' \
  -e 's/\.dataType/.data_type/g' \
  -e 's/\.originMacAddress/.origin_mac_address/g' \
  -e 's/\.targetMacAddress/.target_mac_address/g' \
  -e 's/\.lastHopMacAddress/.last_hop_mac_address/g' \
  -e 's/\.hopCount/.hop_count/g' \
  -e 's/\.epochNum/.epoch_num/g' \
  -e 's/\.seqNum/.seq_num/g' \
  -e 's/\.enrollmentPublicKey/.enrollment_public_key/g' \
  tests/unit/test_mesh_logic.cpp \
  tests/unit/test_replay_cache.cpp \
  tests/unit/test_serial_framing.cpp \
  tests/unit/test_pir_adapter.cpp \
  tests/unit/test_smoke.cpp
```

- [ ] **Step 3: Build test suite**

```bash
cd tests && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Expected: build succeeds with no errors.

- [ ] **Step 4: Run tests**

```bash
ctest --test-dir tests/build --output-on-failure
```

Expected: all existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add main/src/Mesh/Mesh.h main/src/Mesh/Mesh.cpp tests/mocks/mesh_logic_impl.cpp \
        tests/unit/
git commit -m "refactor: migrate Mesh.h to lattice-protocol generated headers (snake_case fields)"
```

---

## Task 5: lattice-nodes — migrate proto source

**Files:**
- Delete: `main/proto/mesh.proto`
- Move: `main/proto/mesh.options` → `main/lib/lattice-protocol/proto/mesh.options`
- Regenerate: `main/src/Mesh/serialization/mesh.pb.h`, `main/src/Mesh/serialization/mesh.pb.c`

**Interfaces:**
- Consumes: `lib/lattice-protocol/proto/mesh.proto` (generated in Task 3)
- Produces: unchanged `mesh.pb.h` / `mesh.pb.c` (nanopb output from the same proto)

- [ ] **Step 1: Move `mesh.options`**

```bash
cp main/proto/mesh.options main/lib/lattice-protocol/proto/mesh.options
git add main/lib/lattice-protocol/proto/mesh.options
```

- [ ] **Step 2: Regenerate `mesh.pb.h` and `mesh.pb.c`**

Nanopb generator must be on PATH. If it isn't installed:
```bash
pip install nanopb
```

Run from the lattice-nodes root:
```bash
python3 -m nanopb_generator \
  main/lib/lattice-protocol/proto/mesh.proto \
  --options-file=main/lib/lattice-protocol/proto/mesh.options \
  --output-dir=main/src/Mesh/serialization
```

- [ ] **Step 3: Verify `mesh.pb.h` is unchanged in content**

```bash
git diff main/src/Mesh/serialization/mesh.pb.h
git diff main/src/Mesh/serialization/mesh.pb.c
```

Expected: no meaningful diff (field tags, struct layout identical). If the generator version causes cosmetic whitespace changes, accept them.

- [ ] **Step 4: Delete old `main/proto/mesh.proto`**

```bash
git rm main/proto/mesh.proto
```

- [ ] **Step 5: Build and run tests**

```bash
cd tests && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add main/src/Mesh/serialization/mesh.pb.h \
        main/src/Mesh/serialization/mesh.pb.c \
        main/lib/lattice-protocol/proto/mesh.options
git commit -m "chore: migrate mesh.proto source to lattice-protocol, regen mesh.pb.h/c"
```

---

## Task 6: lattice-nodes — sendRouteReport() + loop timer

**Files:**
- Modify: `main/project_config.h`
- Modify: `main/src/Mesh/Mesh.h`
- Modify: `main/src/Mesh/Mesh.cpp`
- Modify: `tests/mocks/mesh_logic_impl.cpp`
- Create: `tests/unit/test_route_report.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `OP_ROUTE_REPORT` (0xB3) from `lib/lattice-protocol/c/opcodes.h`; `MESH_TYPE_ROUTE_REPORT` from `lib/lattice-protocol/c/message_types.h`
- Produces: `Mesh::sendRouteReport()` — builds a `MESH_TYPE_ROUTE_REPORT` message with zeroed path, transmits toward master; `ROUTE_REPORT_INTERVAL_MS` config constant

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_route_report.cpp`:

```cpp
#include <gtest/gtest.h>
#include "Mesh/Mesh.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"

using namespace lattice::mesh;

class RouteReportTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  // Set up mesh as non-master with a reachable master peer
  void setupRelayNode(Mesh& mesh, const uint8_t masterMac[6]) {
    mesh.isMaster = false;
    mesh.hasMasterMac = true;
    memcpy(mesh.knownMasterMac, masterMac, 6);
    memcpy(mesh.currentMaster.mac, masterMac, 6);
    mesh.currentMaster.distance = 1;
    memcpy(mesh.currentMaster.nextHop, masterMac, 6);

    // Register master as a live peer so findNextHopToMaster() returns it
    PeerInfo peer{};
    memcpy(peer.mac, masterMac, 6);
    peer.lastSeenMillis = millis();
    mesh.appendPeer(peer);
  }

  mesh_message lastSentMsg() {
    EXPECT_FALSE(espNowSentPackets.empty());
    return *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  }
};

TEST_F(RouteReportTest, SendRouteReport_MessageType) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();

  EXPECT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
}

TEST_F(RouteReportTest, SendRouteReport_PayloadStructure) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh.sendRouteReport();

  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.data[0], OP_ROUTE_REPORT);  // 0xB3
  EXPECT_EQ(sent.data[1], 0);                // path_len = 0 on origin
  // data[2..63] should be zeroed
  for (int i = 2; i < 64; ++i) {
    EXPECT_EQ(sent.data[i], 0) << "data[" << i << "] should be 0";
  }
}

TEST_F(RouteReportTest, SendRouteReport_NotSentByMaster) {
  Mesh mesh;
  mesh.isMaster = true;

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();  // master should be a no-op

  EXPECT_EQ(espNowSentPackets.size(), before);
}
```

- [ ] **Step 2: Run test to verify it fails**

First, register the test in `CMakeLists.txt`. Open `tests/CMakeLists.txt` and add at the end:

```cmake
add_unit_test(test_route_report unit/test_route_report.cpp)
```

Then build and run:

```bash
cd tests && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build 2>&1 | tail -20
```

Expected: build error — `sendRouteReport` not declared.

- [ ] **Step 3: Add `ROUTE_REPORT_INTERVAL_MS` to `project_config.h`**

In `main/project_config.h`, add after `HEALTH_REPORT_INTERVAL_MS`:

```cpp
// Route report interval — 2× health report interval (60 seconds)
constexpr uint32_t ROUTE_REPORT_INTERVAL_MS = HEALTH_REPORT_INTERVAL_MS * 2;
```

- [ ] **Step 4: Declare members and method in `Mesh.h`**

In `main/src/Mesh/Mesh.h`, in the private section, add after `uint32_t lastBeaconMs;`:

```cpp
uint32_t lastRouteReportMs;
```

In the private method declarations, add after `void drainPendingEnrollment();`:

```cpp
void sendRouteReport();
```

In the constructor initializer list in `Mesh.cpp`, add `lastRouteReportMs(0)` (alongside `lastBeaconMs`). Find the constructor initializer list and add it. Example — if the list ends with `lastBeaconMs(0)`, change to:

```cpp
lastBeaconMs(0), lastRouteReportMs(0)
```

- [ ] **Step 5: Implement `sendRouteReport()` in `mesh_logic_impl.cpp`**

Add at the end of `tests/mocks/mesh_logic_impl.cpp` (inside the `lattice::mesh` namespace):

```cpp
void Mesh::sendRouteReport() {
  if (isMaster) return;
  uint8_t data[64] = {};
  data[0] = OP_ROUTE_REPORT;
  data[1] = 0; // path_len — incremented by each relay hop
  transmitCore(adapter_types::UNKNOWN_ADAPTER, data, MESH_TYPE_ROUTE_REPORT);
}
```

- [ ] **Step 6: Add timer call to `loop()` in `Mesh.cpp`**

In `main/src/Mesh/Mesh.cpp`, find the `loop()` method body. Add after the existing `drainPendingEnrollment()` call:

```cpp
if (!isMaster &&
    millis() - lastRouteReportMs >= lattice::config::ROUTE_REPORT_INTERVAL_MS) {
  sendRouteReport();
  lastRouteReportMs = millis();
}
```

- [ ] **Step 7: Build and run tests**

```bash
cd tests && cmake --build build && ctest --test-dir build --output-on-failure -R test_route_report
```

Expected: `SendRouteReport_MessageType`, `SendRouteReport_PayloadStructure`, `SendRouteReport_NotSentByMaster` all PASS.

- [ ] **Step 8: Run full test suite**

```bash
ctest --test-dir tests/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add main/project_config.h main/src/Mesh/Mesh.h main/src/Mesh/Mesh.cpp \
        tests/mocks/mesh_logic_impl.cpp tests/unit/test_route_report.cpp \
        tests/CMakeLists.txt
git commit -m "feat: add sendRouteReport() — MESH_TYPE_ROUTE_REPORT sent every 60s from non-master nodes"
```

---

## Task 7: lattice-nodes — processRouteReport() + dispatch

**Files:**
- Modify: `main/src/Mesh/Mesh.h`
- Modify: `tests/mocks/mesh_logic_impl.cpp`
- Modify: `tests/unit/test_route_report.cpp`

**Interfaces:**
- Consumes: `sendRouteReport()` (Task 6); `transmitCore()`, `externalRecvCallback`, `isMaster`, `deviceMacAddress` from `Mesh`
- Produces: `Mesh::processRouteReport(const mesh_message&)` — relay nodes append own MAC + forward; master delivers to callback

- [ ] **Step 1: Write failing tests**

Add to `tests/unit/test_route_report.cpp` (before the closing `}`):

```cpp
TEST_F(RouteReportTest, ProcessRouteReport_RelayAppendsMAC) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t myMac[6]     = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, myMac, 6); // set known value before setupRelayNode
  setupRelayNode(mesh, masterMac);

  // Build a route report with path_len=1 (one relay MAC already written)
  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = 0;
  msg.hop_count = 1;
  memcpy(msg.origin_mac_address, originMac, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 1; // one relay already written
  // data[2..7] = some relay MAC
  memset(&msg.data[2], 0xAB, 6);

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  ASSERT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
  EXPECT_EQ(sent.data[1], 2); // path_len incremented to 2

  // Our device MAC should be at data[8..13] (index 1 = second relay slot)
  EXPECT_EQ(memcmp(&sent.data[8], myMac, 6), 0);
  EXPECT_EQ(sent.hop_count, 2);
}

TEST_F(RouteReportTest, ProcessRouteReport_MasterDeliversToCallback) {
  Mesh mesh;
  mesh.isMaster = true;

  bool callbackFired = false;
  mesh_message received{};
  mesh.linkDataRecvCallback([&](mesh_message m) {
    callbackFired = true;
    received = m;
  });

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 1;

  mesh.processRouteReport(msg);

  EXPECT_TRUE(callbackFired);
  EXPECT_EQ(received.data[0], OP_ROUTE_REPORT);
}

TEST_F(RouteReportTest, ProcessRouteReport_PathFullDropsMessage) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 10; // path full — max 10 relay MACs

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no message sent
}

TEST_F(RouteReportTest, ProcessRouteReport_MalformedOpcodeDropsMessage) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = 0xFF; // wrong opcode
  msg.data[1] = 0;

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no message sent
}

TEST_F(RouteReportTest, DrainRecvQueue_DispatchesRouteReport) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 0;

  // Directly push to recv queue (UNIT_TEST exposes all members)
  uint8_t srcMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t nextHead = (mesh.recvQueueHead + 1) % Mesh::RECV_QUEUE_SIZE;
  memcpy(mesh.recvQueue[mesh.recvQueueHead].srcMac, srcMac, 6);
  mesh.recvQueue[mesh.recvQueueHead].msg = msg;
  mesh.recvQueueHead = nextHead;

  size_t before = espNowSentPackets.size();
  mesh.drainRecvQueue();

  EXPECT_GT(espNowSentPackets.size(), before); // relayed the message
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd tests && cmake --build build 2>&1 | tail -10
```

Expected: build error — `processRouteReport` not declared.

- [ ] **Step 3: Declare `processRouteReport` in `Mesh.h`**

In `main/src/Mesh/Mesh.h`, in the private methods section, add after `sendRouteReport()`:

```cpp
void processRouteReport(const mesh_message& msg);
```

- [ ] **Step 4: Implement `processRouteReport()` in `mesh_logic_impl.cpp`**

Add after `sendRouteReport()` (inside `lattice::mesh` namespace):

```cpp
void Mesh::processRouteReport(const mesh_message& msg) {
  // Verify opcode
  if (msg.data[0] != OP_ROUTE_REPORT) {
    Logger::logln("MESH", "processRouteReport: bad opcode, dropping", LogLevel::LOG_WARN);
    return;
  }

  if (isMaster) {
    // Terminal endpoint — deliver to server via external callback
    if (externalRecvCallback) externalRecvCallback(msg);
    return;
  }

  // Relay node: append own MAC to path and forward toward master
  uint8_t path_len = msg.data[1];
  if (path_len >= 10) {
    Logger::logln("MESH", "processRouteReport: path full, dropping", LogLevel::LOG_WARN);
    return;
  }

  mesh_message relay = msg;
  memcpy(&relay.data[2 + path_len * 6], deviceMacAddress, 6);
  relay.data[1]++;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  transmitCore(relay.data_type, relay.data, MESH_TYPE_ROUTE_REPORT, &relay);
}
```

- [ ] **Step 5: Add dispatch case to `drainRecvQueue()` in `mesh_logic_impl.cpp`**

Find the `switch (msg.message_type)` block in `drainRecvQueue()`. Add a new case before `default:`:

```cpp
case MESH_TYPE_ROUTE_REPORT:
  processRouteReport(msg);
  break;
```

- [ ] **Step 6: Build and run route report tests**

```bash
cd tests && cmake --build build && ctest --test-dir build --output-on-failure -R test_route_report
```

Expected: all 8 route report tests PASS.

- [ ] **Step 7: Run full test suite**

```bash
ctest --test-dir tests/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add main/src/Mesh/Mesh.h \
        tests/mocks/mesh_logic_impl.cpp \
        tests/unit/test_route_report.cpp
git commit -m "feat: add processRouteReport() — relay appends MAC in-transit, master delivers to callback"
```
