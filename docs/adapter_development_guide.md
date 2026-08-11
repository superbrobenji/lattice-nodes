# Adapter Development Guide

This guide explains how to add new adapters to the Lattice firmware and how to
change the default adapter used on first boot / in dev mode.

## Table of Contents

1. [Adapter Architecture Overview](#adapter-architecture-overview)
2. [Adding a New Adapter](#adding-a-new-adapter)
3. [Changing the Default Adapter](#changing-the-default-adapter)
4. [Testing Your New Adapter](#testing-your-new-adapter)

## Adapter Architecture Overview

Every adapter is a subclass of `lattice::adapter::Adapter`
(`firmware/main/src/adapter/Adapter.h` / `.cpp`), the abstract base that owns
the shared mesh-message plumbing, health-report machinery, and control-opcode
dispatch. Two concrete adapters exist today:

- `PirAdapter` (`firmware/main/src/adapter/pir/PirAdapter.{h,cpp}`) — drives a
  PIR motion sensor.
- `SerialAdapter` (`firmware/main/src/adapter/serial/SerialAdapter.{h,cpp}`) —
  bridges the master node's USB/UART link to the server and forwards every
  mesh message it receives.

### The `adapter_types` enum

```cpp
// firmware/main/src/adapter/Adapter.h
enum adapter_types : int32_t {
  UNKNOWN_ADAPTER = 0,
  SERIAL_ADAPTER = 1,
  PIR_ADAPTER = 2,
};
```

It is deliberately unscoped (plain `enum`, not `enum class`) so it can be
compared/assigned against the generated wire struct's `int32_t data_type`
field without explicit casts in either direction. There is no `LED_ADAPTER`
or `WIFI_ADAPTER` — those never existed in this enum. (The vendored,
generated wire-protocol header `firmware/main/lib/lattice-protocol/c/adapter_types.h`
does reserve `ADAPTER_TYPE_LED=3` and `ADAPTER_TYPE_RELAY=4` at the protocol
level for future use, but that is a separate C header the firmware's C++
`adapter_types` enum does not include or use — there is no firmware-side LED
or relay adapter class today. Don't conflate the two when reading the code.)

### Base class API

```cpp
class Adapter {
protected:
  uint8_t _pin;                 // GPIO 0-39
  adapter_types _adapterType;   // starts UNKNOWN_ADAPTER; set by the subclass ctor
  typedef void (*TransmitPtr)(adapter_types, const uint8_t*);
  TransmitPtr mesh_transmit_fn; // installed by main.cpp via setTransmitFn()

public:
  explicit Adapter(uint8_t pin);   // NOTE: type is NOT a ctor parameter
  virtual ~Adapter() = default;

  adapter_types getAdapterType() const;
  void sendDataThroughMesh(adapter_types type, const uint8_t* data);
  void setTransmitFn(TransmitPtr fn);

  virtual bool init() = 0;  // pure virtual — hardware bring-up
  virtual void loop() = 0;  // pure virtual — called every housekeeping tick (~100 Hz, non-blocking)

  // NOT virtual — shared entry point for every inbound mesh message.
  void onMeshData(const lattice::mesh::mesh_message& message);

protected:
  // Optional override — base implementation is a no-op.
  virtual void onMeshDataImpl(const lattice::mesh::mesh_message& message);
  // ... health-report + control-op dispatch machinery, see below
};
```

The constructor takes **only a pin**, not an adapter type — this is the most
common mistake when porting old examples. Each subclass sets
`_adapterType` itself, in its own constructor body, after the base class has
already initialized it to `UNKNOWN_ADAPTER`:

```cpp
PirAdapter::PirAdapter(uint8_t pin)
    : Adapter(pin), _pir(pin), _lastTrigger(0), _state(PirState::IDLE),
      _interruptEnabled(false), _initialized(false) {
  _adapterType = adapter_types::PIR_ADAPTER;
}
```

### Inbound message filtering: `onMeshData` vs. `onMeshDataImpl`

`onMeshData()` is the single, non-virtual entry point every adapter receives
mesh traffic through (called from `main.cpp`'s `dataRecvCallback`). It always
runs the shared control-opcode dispatch table first (see below) for
`SERIAL_ADAPTER`-typed control messages, regardless of the receiving node's
own adapter type — so any node can be reconfigured no matter what it
currently is. For everything else, it filters by
`message.dataType == _adapterType` before calling the virtual
`onMeshDataImpl()` hook — **except** `SerialAdapter`, which receives every
message unfiltered, because it's the master's uplink and must forward all
node telemetry to the server regardless of type.

### Shared control-op dispatch table

```cpp
using ControlOpFn = void (Adapter::*)(const mesh_message&, bool);
struct ControlOpEntry { uint8_t opcode; ControlOpFn handler; };
static const ControlOpEntry kControlOps[] = {
    {OP_CONFIG_SET,   &Adapter::opConfigSet},
    {OP_NODE_ID_SET,  &Adapter::opNodeIdSet},
    {OP_HEALTH_REQ,   &Adapter::opHealthReq},
    {OP_TX_POWER_SET, &Adapter::opTxPowerSet},
};
```

`dispatchControlOp(message, rebroadcastOnMaster)` looks up `message.data[0]`
in this table and invokes the matching handler if found. It's reached from
two call sites: `Adapter::onMeshData()` (the mesh path, always
`rebroadcastOnMaster=false`) and `SerialAdapter::handleCompleteFrame()` (the
direct-serial path, `rebroadcastOnMaster=true`). Handlers, all implemented
once in the base class so no per-adapter code is needed:

- `opConfigSet` — wire `[0xC1][6B targetMac][1B adapterType]`. If targeted at
  this node, saves the new adapter type to EEPROM via
  `AdapterFactory::saveAdapterTypeToEEPROM()` and calls `esp_restart()` to
  pick it up.
- `opNodeIdSet` — wire `[0xC0][6B targetMAC][1B nodeId]`. If targeted at this
  node, saves the node ID to EEPROM.
- `opHealthReq` — no-op unless `_adapterType == SERIAL_ADAPTER`; otherwise
  triggers `sendSelfHealthReport()`.
- `opTxPowerSet` — no-op unless `SERIAL_ADAPTER`; otherwise validates a
  0-2 preset, saves it to EEPROM, applies it via `esp_wifi_set_max_tx_power`,
  and — only when `rebroadcastOnMaster` is true — re-broadcasts the change to
  the rest of the mesh (this guard prevents a re-broadcast loop when the
  change arrived *via* the mesh in the first place).

### Health-report builders

- `void buildHealthFrame(uint8_t opcode, uint8_t* buf, size_t bufsize) const`
  — fills `[1B opcode][1B adapterType][6B MAC][4B little-endian
  uptime-seconds]` (12 bytes total); no-op if `bufsize < 12`. Only fills the
  buffer — the caller owns transmission, since that differs by adapter (a PIR
  node routes the frame through the mesh via `sendDataThroughMesh`; the
  serial/master adapter injects it directly into its own outbound pipeline).
- `void sendSelfHealthReport() const` — builds an `OP_HEALTH_REPORT` frame
  and delivers it via `Mesh::transmitSelfOriginated`. Meaningful only for the
  serial-attached master.
- `bool healthTickDue(uint64_t now)` — returns true once per
  `HEALTH_REPORT_INTERVAL_MS` (30000 ms, `project_config.h`) and auto-resets
  the internal timer when it fires.
- `void resetHealthTick(uint64_t now)` — manual reset, used by
  `SerialAdapter` when a hop-count change needs an out-of-cycle report.

### Message flow

**Incoming:** `main.cpp`'s `dataRecvCallback` calls
`adapter->onMeshData(message)` → base class runs the control-op dispatch,
then filters by adapter type → calls `onMeshDataImpl()` if the type matches
(or unconditionally for `SerialAdapter`).

**Outgoing:** an adapter calls `sendDataThroughMesh(type, data)` (inherited,
non-virtual), which invokes the `mesh_transmit_fn` installed by `main.cpp` at
boot (`adapter->setTransmitFn(&Mesh::transmit)`) — or logs
`err::fail(CONFIG, ADAPTER, 1)` if no transmit function was set.

## Adding a New Adapter

This walkthrough mirrors `PirAdapter`, the simplest concrete adapter in the
tree, and builds a hypothetical `MyNewAdapter` as the running example.

### Step 1: Create the directory and files

Create a new **lowercase** directory under `firmware/main/src/adapter/`,
matching the existing `pir/` and `serial/` layout — there is no capitalized
`Adapter/` parent folder and no underscore in adapter class names:

```
firmware/main/src/adapter/mynew/MyNewAdapter.h
firmware/main/src/adapter/mynew/MyNewAdapter.cpp
```

(The old capitalized convention, `src/Adapter/PIR_Adapter/PIR_Adapter.h`,
does not exist anywhere in this codebase — don't recreate it.)

### Step 2: Write the header

```cpp
#ifndef MY_NEW_ADAPTER_H
#define MY_NEW_ADAPTER_H

#include "src/adapter/Adapter.h"
// #include your hardware driver header here, e.g.:
// #include "src/hardware/input/Pir.h"

namespace lattice {
namespace adapter {

class MyNewAdapter : public Adapter {
public:
  explicit MyNewAdapter(uint8_t pin);
  bool init() override;
  void loop() override;
  void onMeshDataImpl(const lattice::mesh::mesh_message& message) override; // optional

private:
  // hardware::MyDriver _myHw;  // constructed with the same pin
  bool _initialized = false;
};

} // namespace adapter
} // namespace lattice

#endif
```

- The constructor signature **must** be exactly `explicit MyNewAdapter(uint8_t pin)`
  — this matches `AdapterFactory::createAdapter`'s uniform call site.
- `init()` and `loop()` are pure virtual in the base and must be overridden.
- `onMeshDataImpl()` is optional — the base class already no-ops it.

### Step 3: Write the constructor and lifecycle methods

```cpp
#include "MyNewAdapter.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"

namespace lattice {
namespace adapter {

MyNewAdapter::MyNewAdapter(uint8_t pin)
    : Adapter(pin) /*, _myHw(pin) */ {
  _adapterType = adapter_types::MY_NEW_ADAPTER;
}

bool MyNewAdapter::init() {
  if (_initialized) {
    LATTICE_LOGLN("MyNewAdapter", "Warning: Already initialized.", LogLevel::LOG_WARN);
    return true;
  }
  // if (!_myHw.init()) {
  //   lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG,
  //                      lattice::core::ModuleDigit::ADAPTER, 1,
  //                      "MyNewAdapter: hardware failed to initialize.");
  //   return false;
  // }
  _initialized = true;
  return true;
}

void MyNewAdapter::loop() {
  // Non-blocking — called ~100 Hz from main.cpp's housekeeping task.
  // Never delay()/block in here.

  // To send data:
  // uint8_t data[64] = {0};
  // sendDataThroughMesh(_adapterType, data);

  // For periodic health reports:
  // if (healthTickDue(nowMs)) {
  //   uint8_t buf[12];
  //   buildHealthFrame(OP_NODE_HEALTH, buf, sizeof(buf));
  //   sendDataThroughMesh(SERIAL_ADAPTER, buf); // route through the mesh to reach the master
  // }
}

} // namespace adapter
} // namespace lattice
```

Notes:

- The base `Adapter(uint8_t pin)` constructor does **not** take a type —
  assign `_adapterType` in the derived constructor body, after the base has
  constructed (it starts as `UNKNOWN_ADAPTER`).
- Non-master adapters (anything other than `SerialAdapter`) must route health
  reports through the mesh via `sendDataThroughMesh(SERIAL_ADAPTER, data)` —
  only `SerialAdapter` itself uses the direct self-originated transmit path
  (`sendSelfHealthReport()`), because it's the only adapter with a physical
  serial link to the server.
- Follow `PirAdapter::init()`'s pattern: guard against double-initialization,
  report hardware failures via `lattice::err::fail(...)`, and set an
  `_initialized` flag.

### Step 4: Register the new enum value

In `firmware/main/src/adapter/Adapter.h`, add the next free integer to
`adapter_types`:

```cpp
enum adapter_types : int32_t {
  UNKNOWN_ADAPTER = 0,
  SERIAL_ADAPTER = 1,
  PIR_ADAPTER = 2,
  MY_NEW_ADAPTER = 3, // next free value
};
```

### Step 5: Add a default pin constant

In `firmware/main/src/adapter/AdapterFactory.h`:

```cpp
static constexpr uint8_t MY_NEW_ADAPTER_DEFAULT_PIN = <pin>;
```

(Default pin constants for adapters live in `AdapterFactory.h`, not
`project_config.h` — e.g. `PIR_ADAPTER_DEFAULT_PIN = 27` is defined there
today, alongside the unused `SERIAL_ADAPTER_DEFAULT_PIN = 255` sentinel.)

Then add a case to the switch in `AdapterFactory::getDefaultPinForAdapter`
(`firmware/main/src/adapter/AdapterFactory.cpp`):

```cpp
case adapter_types::MY_NEW_ADAPTER:
  return MY_NEW_ADAPTER_DEFAULT_PIN;
```

### Step 6: Register with the factory

In `firmware/main/src/adapter/AdapterFactory.cpp`, include the new header
alongside the existing adapter includes:

```cpp
#include "src/adapter/pir/PirAdapter.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/adapter/mynew/MyNewAdapter.h" // add this
```

And add a case to `AdapterFactory::createAdapter`'s switch:

```cpp
Adapter* AdapterFactory::createAdapter(adapter_types type, uint8_t pin) {
  switch (type) {
  case adapter_types::PIR_ADAPTER:
    return new PirAdapter(pin);
  case adapter_types::SERIAL_ADAPTER:
    return new SerialAdapter(pin);
  case adapter_types::MY_NEW_ADAPTER: // add this
    return new MyNewAdapter(pin);
  default:
    lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG,
                       lattice::core::ModuleDigit::ADAPTER, 2,
                       "AdapterFactory: Unknown adapter type");
    return nullptr;
  }
}
```

### Step 7: GPIO boot configuration

There is no per-component `pinMode()`-at-construction-time anymore.
`GpioInput::init()` / `GpioOutput::init()` only validate the pin and set an
`_initialized` flag — actual pin-mode and pull-up/pull-down configuration is
centralized in `main.cpp`'s `initDrivers()`, applied once at boot via bundled
`gpio_config_t` calls (grouped by required pull configuration: outputs,
pull-down inputs, pull-up inputs). If your adapter's hardware needs a
specific pull direction or output mode, add its pin to the appropriate group
there — do not configure pin mode from inside your adapter or its driver.

### Step 8: Persistence

Adapter type is persisted in EEPROM via
`lattice::eeprom::loadAdapterType()` / `saveAdapterType()`, and can be
changed at runtime by sending the generic `OP_CONFIG_SET` control opcode —
handled entirely by the shared `Adapter::opConfigSet` (see "Shared control-op
dispatch table" in the architecture overview above), which triggers
`esp_restart()` to pick up the new type. **No per-adapter persistence code is
needed.**

## Changing the Default Adapter

The adapter used on first boot (fresh/unset EEPROM) and whenever `DEV_MODE`
is enabled is controlled by a single constant:

```cpp
// firmware/main/project_config.h
// IMPORTANT: For server communication via USB, MUST be SERIAL_ADAPTER
constexpr lattice::adapter::adapter_types DEFAULT_ADAPTER =
    lattice::adapter::adapter_types::SERIAL_ADAPTER;
```

To change it, edit `DEFAULT_ADAPTER` in `firmware/main/project_config.h`.
Keep the master node's `DEFAULT_ADAPTER` set to `SERIAL_ADAPTER` — that node
needs the direct USB/UART link to the server. Non-master nodes (e.g. PIR
sensor nodes) should use `PIR_ADAPTER` or whatever adapter matches their
hardware.

There are two other ways an adapter type takes effect, without touching
source:

- **At runtime, over the mesh or direct serial**: send the `OP_CONFIG_SET`
  (`0xC1`) control opcode targeted at the node's MAC with the desired
  `adapter_types` value as payload. The node saves it to EEPROM and restarts
  (see `Adapter::opConfigSet` above).
- **By clearing EEPROM**: use the device's **reset button** (hold 5 seconds
  to arm, then hold again within 3 seconds to confirm) — this calls
  `lattice::eeprom::clearAll()` and restarts (see
  `ButtonHandler::tickReset()` in `firmware/main/src/app/ButtonHandler.h`).
  On next boot, an unset (`0xFF`) adapter-type byte falls back to
  `PIR_ADAPTER` (`AdapterFactory::adapterTypeFromEEPROM`'s fresh-EEPROM
  default), not whatever `DEFAULT_ADAPTER` says — `DEFAULT_ADAPTER` only
  applies in `DEV_MODE`.

Never call raw EEPROM/NVS primitives directly, and never edit
`AdapterFactory::createAdapter`/`createFromEEPROM` to hardcode a type — route
all EEPROM I/O through the `lattice::eeprom` namespace
(`firmware/main/src/persistence/eeprom/`) so `DEV_MODE` and address/key
constants stay respected.

## Testing Your New Adapter

Adapters are tested on the host via the GoogleTest-based unit-test suite
under `tests/unit/`, using the mocks in `tests/mocks/` — no hardware or
ESP-IDF build is required.

### 1. Build and run the host test suite

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
```

(See the top-level [`README.md`](../README.md#running-unit-tests) for the
full test-running instructions, including the end-to-end simulation suite.)

### 2. Add a new test file and register it

Use `tests/unit/test_pir_adapter.cpp` as the pattern to copy — it constructs
a `PirAdapter` directly against the mock transmit function and mocked
EEPROM/time helpers, and exercises both motion-triggered sends and periodic
health reports:

```cpp
#include <gtest/gtest.h>
#include "adapter/pir/PirAdapter.h"
#include "adapter/AdapterFactory.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "persistence/eeprom/EepromCore.h"
```

At minimum, a new adapter's test file should cover:

- **Type identity** — `getAdapterType()` returns the expected `adapter_types`
  value after construction.
- **`init()`** — succeeds on first call; the "already initialized" guard
  returns `true` without re-running hardware bring-up on a second call.
- **Outbound data** — whatever your adapter transmits on its primary trigger
  (e.g. `PirAdapter`'s motion-detected `ADAPTER_DATA` frame) is sent via
  `sendDataThroughMesh` with the correct `adapter_types` and payload bytes,
  captured through a mock transmit function (see `captureTransmit()` in
  `test_pir_adapter.cpp`).
- **Periodic health reports** — a health frame fires after
  `HEALTH_REPORT_INTERVAL_MS` (30000 ms) via the inherited `healthTickDue()`
  tick, with the correct opcode/adapter-type/MAC/uptime bytes (see
  `PIRHealthTest::SendsNodeHealthAfter30s` / `DoesNotSendNodeHealthBefore30s`
  in `test_pir_adapter.cpp` for the exact byte-layout assertions).
- **Inbound filtering** — `onMeshDataImpl()` only runs for messages whose
  `dataType` matches your adapter's type (exercised through `onMeshData()`,
  not called directly).

Register the new file in `tests/CMakeLists.txt` alongside the existing
`add_unit_test(...)` calls:

```cmake
add_unit_test(test_my_new_adapter unit/test_my_new_adapter.cpp)
```

### 3. Common issues

| Issue | Likely cause |
|-------|--------------|
| Link errors on host build | New adapter's `.cpp` not added to `FIRMWARE_SOURCES` in `tests/CMakeLists.txt` |
| Compilation errors | Missing `#include`s, or constructor signature doesn't match `explicit MyNewAdapter(uint8_t pin)` |
| Adapter never receives mesh messages | `dataType` sent by the peer doesn't match `_adapterType`, or the message is going through `onMeshData()` filtering as expected — check whether your adapter should instead receive everything unfiltered (only `SerialAdapter` does) |
| Health report never fires in a test | Test must call `advanceMillis()` (from `time_mock.h`) past `HEALTH_REPORT_INTERVAL_MS` before calling `loop()` |
| EEPROM state leaking between tests | Call `EEPROM.reset()` and re-run `lattice::eeprom::init()` in `SetUp()`, as `test_pir_adapter.cpp` does |

Once the host-test suite passes, verify on real hardware: from `firmware/`,
run `idf.py build flash monitor` (the standard ESP-IDF invocation; not
independently documented elsewhere in this repo), watch the serial log for
the adapter's initialization message, and confirm mesh traffic and health
reports appear as expected.
