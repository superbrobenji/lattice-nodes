# Repo Restructure & Code Cleanup Design

**Date:** 2026-07-13  
**Scope:** Full directory restructure, file decomposition, naming normalisation  
**Constraint:** Zero functional change — restructure only

---

## Goals

- Directory names mirror C++ namespaces (lowercase)
- File naming consistent (PascalCase, no SCREAMING_SNAKE)
- `Mesh.cpp` decomposed into focused, independently testable units
- `main.ino` reduced to wiring-only (~150 lines)
- `SerialAdapter` decomposed: framing logic separated from adapter lifecycle
- `CMakeLists.txt` and all `#include` paths updated throughout

---

## Target Directory Structure

```
main/
  main.ino                         (~150 lines — wiring only)
  project_config.h                 (unchanged — Arduino requires root location)
  lib/                             (unchanged — git submodule)
  src/
    app/                           NEW — extracted from main.ino
      BootManager.h                reset reason check, WDT reboot counter, halt-on-loop
      DisplayManager.h             7-seg enrolled/unenrolled/master state machine
      ButtonHandler.h              config + reset button hold/2-step-wipe logic

    mesh/                          was Mesh/ — lowercase matches lattice::mesh namespace
      Mesh.h/.cpp                  thin orchestrator: radio init, loop(), routing dispatch
      PeerRegistry.h/.cpp          peer list CRUD + EEPROM load/save/find/add/remove
      ReplayCache.h                header-only: replay protection struct + isReplay()
      MeshCrypto.h                 header-only: derivePeerLMK + registerPeerWithEspNow (free fns)
      Enrollment.h/.cpp            enrollment protocol: send/processRequest/processJoinAck/enroll
      serialization/               unchanged — generated nanopb files + runtime
        mesh.pb.c / mesh.pb.h
        nanopb/

    adapter/                       was Adapter/ — lowercase matches lattice::adapter namespace
      Adapter.h/.cpp               base class (content unchanged)
      AdapterFactory.h/.cpp        factory (content unchanged)
      pir/                         was PIR_Adapter/
        PirAdapter.h/.cpp          was PIR_Adapter.h/.cpp — class renamed PirAdapter
      serial/                      was Serial_Adapter/
        SerialAdapter.h/.cpp       was Serial_Adapter.h/.cpp — class renamed SerialAdapter
        SerialFraming.h/.cpp       NEW — encode/decode/injectByte extracted from SerialAdapter

    hardware/                      unchanged — already well structured
      input/  Button, GpioInput, Pir
      output/ GpioOutput, Led, SevenSegDisplay

    logging/                       was core/ — name reflects sole occupant
      Logger.h/.cpp

    error/                         unchanged
      Error.h / ErrorCodes.h / ErrorCore.h/.cpp

    network/                       unchanged
      MacAddress.h

    persistence/
      EepromManager.h/.cpp         was EEPROM_Manager — drop SCREAMING_SNAKE prefix
```

---

## Mesh Decomposition

`Mesh` becomes a thin orchestrator by composing extracted concerns as member objects:

```cpp
class Mesh {
  PeerRegistry  peers;      // peer array, EEPROM load/save/find
  Enrollment    enrollment; // keypair, enrolled flag, TOFU MACs, join state
  ReplayCache   replay;     // replay cache array, bootEpoch, txSeqNum

  // Mesh retains: recv ring buffer, radio setup/teardown,
  //               routing dispatch, beacon relay, loop()
};
```

### `PeerRegistry` (`mesh/PeerRegistry.h/.cpp`)
Owns: `peerMacs[]`, `peerCount`, peer EEPROM serialisation.  
Provides: `find()`, `append()`, `remove()`, `loadFromEEPROM()`, `saveToEEPROM()`, `isPeerInRange()`, `findNextHopToMaster()`.  
Depends on: `EepromManager`, `MacAddress`.

### `Enrollment` (`mesh/Enrollment.h/.cpp`)
Owns: `devicePrivateKey[32]`, `devicePublicKey[32]`, `enrolledFlag`, `knownMasterMac`, `knownMasterMacSecondary`, `hasMasterMac`, `hasMasterMacSecondary`, `_pendingEnrollmentRelay`, `_enrollmentRelayFn`.  
Provides: `sendRequest()`, `processRequest()`, `processJoinAck()`, `enrollPeer()`, `isEnrolled()`, `getPublicKey()`, `setRelayFn()`, `drainPendingRelay()`.  
Depends on: `MeshCrypto`, `EepromManager`.

### `ReplayCache` (`mesh/ReplayCache.h` — header-only)
Owns: `replayCache[16]`, `bootEpoch`, `txSeqNum`, `lastRelayedEpoch`, `lastRelayedSeqNum`.  
Provides: `isReplay(const mesh_message&)`, `nextSeq()`, `init(uint32_t epoch)`.  
No external dependencies.

### `MeshCrypto` (`mesh/MeshCrypto.h` — header-only free functions)
```cpp
namespace lattice::mesh::crypto {
  void derivePeerLMK(const uint8_t* ownPriv, const uint8_t* peerPub, uint8_t* lmk16Out);
  void registerPeerWithEspNow(const uint8_t mac[6], const uint8_t* ownPriv, const uint8_t* peerPub);
  void generateKeypair(uint8_t* priv32Out, uint8_t* pub32Out);
}
```

---

## `app/` Layer

Three header-only classes extracted from `main.ino`. Header-only because they coordinate existing objects — no new translation units needed.

**`BootManager`** — wraps the reset-reason block in `setup()`:
- `static void check(EepromManager&)` — reads reset reason, increments WDT counter, halts if ≥5 consecutive WDT resets.

**`DisplayManager`** — wraps the 7-seg `static` block in `loop()`:
- `static void tick(SevenSegDisplay&, bool enrolled, bool isMaster, uint8_t nodeId)` — handles enrolled/unenrolled/master display states + flash timer.

**`ButtonHandler`** — wraps config + reset button state machines in `loop()`:
- `static void tick(Button& cfg, Button& rst, Mesh&, EepromManager&, Led&, Led&)` — role toggle + 2-step EEPROM wipe.

`main.ino` after extraction: globals → `setup()` wiring → `loop()` dispatch. Enrollment broadcast timer (5 lines) stays inline — too small to extract.

---

## `SerialAdapter` Decomposition

**`SerialFraming`** (`adapter/serial/SerialFraming.h/.cpp`) extracts:
- `encode(const mesh_message&, uint8_t* out, size_t maxLen) → size_t`
- `decode(const uint8_t* data, size_t len, mesh_message& out) → bool`
- `injectByte(uint8_t) → bool` — COBS/framing state machine

`SerialAdapter` retains: `init()`, `loop()`, `sendHealthReport()`, `onMeshDataImpl()`, `handleCompleteFrame()`, `relayEnrollmentToServer()`. Drops to ~200 lines.

---

## Renames

| Old name | New name | Type |
|----------|----------|------|
| `src/Mesh/` | `src/mesh/` | directory |
| `src/Adapter/` | `src/adapter/` | directory |
| `src/Adapter/PIR_Adapter/` | `src/adapter/pir/` | directory |
| `src/Adapter/Serial_Adapter/` | `src/adapter/serial/` | directory |
| `src/core/` | `src/logging/` | directory |
| `PIR_Adapter` (class + files) | `PirAdapter` | class + file |
| `Serial_Adapter` (class + files) | `SerialAdapter` | class + file |
| `EEPROM_Manager` (class + files) | `EepromManager` | class + file |

---

## Build Changes

**`tests/CMakeLists.txt`** — update all existing source paths (renames); add three genuinely new `.cpp` files:
- `../main/src/mesh/PeerRegistry.cpp`  ← new
- `../main/src/mesh/Enrollment.cpp`    ← new
- `../main/src/adapter/serial/SerialFraming.cpp`  ← new

All other entries are path updates only (e.g. `EEPROM_Manager.cpp` → `EepromManager.cpp`).

**`tests/mocks/mesh_logic_impl.cpp`** — update to reflect new Mesh member structure (`mesh.peers`, `mesh.enrollment`, `mesh.replay`).

**`UNIT_TEST` friend access** — moves from blanket `#ifdef UNIT_TEST public:` in `Mesh.h` to targeted friend declarations in `PeerRegistry.h`, `Enrollment.h`, `ReplayCache.h`.

---

## What Does NOT Change

- `project_config.h` location and content (except `#include` path updates)
- `lib/lattice-protocol/` (submodule, untouched)
- `src/hardware/` (already clean)
- `src/error/` (already clean)
- `src/network/MacAddress.h`
- `src/mesh/serialization/` + nanopb (generated/vendored)
- All logic, algorithms, protocols — zero functional change

---

## Risk Areas

1. **`mesh_logic_impl.cpp` mock** — provides test-only Mesh implementation; must be updated to match new member structure.
2. **`project_config.h`** includes `src/Adapter/Adapter.h` and `src/core/Logger.h` — paths update.
3. **`IRAM_ATTR` callbacks** — stay in `Mesh.cpp`; no movement of ISR-context code.
4. **Static method refs** — `Serial_Adapter::relayEnrollmentToServer` → `SerialAdapter::relayEnrollmentToServer` (one call site in `main.ino`).
