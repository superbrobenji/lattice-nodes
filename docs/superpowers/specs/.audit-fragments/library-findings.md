# Library findings (Phase A Clean-Code Audit — Task 4)

Scope: repo-wide grep sweep of `firmware/main/src/**/*.{h,cpp}` for hand-rolled
patterns that commonly have a library equivalent (ring buffers, state
machines, fixed-size hash/lookup tables, checksum/CRC routines), each
evaluated with a flash/RAM estimate that accounts for what a candidate
library actually links in — not its advertised size — per the Phase J
lesson (`a3eb997`: swapping mbedtls for libsodium measured **+82–92.5 KB**,
not the predicted −40 to −60 KB, because `sodium_init()` link-reaches
essentially the whole library regardless of how few primitives are called).

## Library candidates

| ID | Hand-rolled code | Candidate library | Est. flash/RAM delta | Verdict |
|---|---|---|---|---|
| LIB-01 | `RouteTable::entries[16]`, `NeighborTable::entries[8]`, `PeerRegistry::peerMacs[MAX_PEERS]`, `ReplayCache::cache[12]`, `E2EKeyStore::entries[≤10]` — five independent fixed-size linear-scan-by-MAC arrays, already deduped onto a shared non-templated helper (`firmware/main/src/network/mac_table.h`, Phase H2 item Y: `mac_table::find`/`evict_oldest_by_ts`) | A hash table / hash-map component (no ESP-IDF-native small hash-table library exists; would mean pulling in `std::unordered_map` or a third-party header-only hash map) | **None credible — negative expected value, no spike needed.** All five tables cap at N ∈ {8, 10, 12, 16, MAX_PEERS}; a linear scan over ≤16 six-byte MAC compares is a handful of cycles and zero extra code. `std::unordered_map` pulls in dynamic allocation, a hash function, bucket-array bookkeeping, and (per-instantiation, since these are 5 distinct entry types) that cost multiplies by five unless further genericized — `mac_table.h`'s own comment states it deliberately avoided templates "to avoid a template instantiated per entry type bloating flash." A hash table is worse on both axes (flash and RAM) at this scale. | **Reject** |
| LIB-02 | `crc16()` in `firmware/main/src/persistence/EepromManager.cpp:30-48` — hand-rolled non-reflected CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`), used only to self-check a persisted 64-byte keypair (write vs. read, no external interop) | `esp_rom_crc16_le` / `esp_rom_crc16_be` (ESP-IDF ROM CRC — already resident in every ESP32 binary at effectively zero marginal flash cost, unlike a linked library) | **Already evaluated — no new estimate needed.** Phase I Task 4 (item UU) considered this exact swap and rejected it: neither ROM variant is a bit-exact match for this specific non-reflected CRCCITT-FALSE without an unverified pre/post-invert transform (`~crc16_be(~init, ...)`), and since the value is self-referential (round-trips through this device's own NVS only) the correctness risk of an unverified transform outweighs the near-zero flash win available. See `EepromManager.cpp:31-40` for the team's own reasoning, still accurate — this sweep found no new evidence to revisit it. | **Reject (re-confirmed prior finding)** |
| LIB-03 | `PirAdapter::PirState` (`IDLE, PENDING_SEND, COOLDOWN`, `firmware/main/src/adapter/pir/PirAdapter.h:38`) and `SerialFraming::FrameState` (`AwaitingLen1, AwaitingLen2, AwaitingPayload`, `firmware/main/src/adapter/serial/SerialFraming.h:36`, dispatched via `switch` in `SerialFraming.cpp:162-204`) — two 3-state hand-rolled FSMs (`enum class` + `switch`) | A generic FSM framework (e.g. tinyfsm, Boost.SML — both header-only, template-heavy; neither is ESP-IDF-native) | **Negative on its face — no spike needed.** `enum class` + `switch` is already the zero-overhead idiomatic embedded C++ pattern: no vtable, no function-pointer table, no per-state object. A template-based FSM library adds an entire templated dispatch mechanism (and its own instantiation-per-state-machine flash cost, the same bloat pattern `mac_table.h` was written to avoid) to formalize something a 3-case switch already expresses correctly and minimally. Also: `PirState` was itself the *product* of a Phase G audit collapse (item K, two overlapping bools → one enum) — the pattern here is the target state of a prior simplification, not a leftover needing one. | **Reject** |
| LIB-04 | ESP-NOW receive queue (`Mesh.h`/`Mesh.cpp`) and enrollment relay queue (`Enrollment.h`/`.cpp`) — grep for "ring" surfaces these, but they are **not currently hand-rolled** | N/A — already on FreeRTOS's native `xRingbufferCreateStatic`/`RINGBUF_TYPE_NOSPLIT` | N/A — already done | **N/A (already resolved)** — Phase I Task 8 (item OO) replaced a hand-rolled head/tail/count SPSC array with the native FreeRTOS static ring buffer for both queues. Flagging explicitly so Task 5 doesn't re-list this as an open candidate; the grep's "ring" hit is residual naming/comments, not hand-rolled code. |
| LIB-05 | **Inverse case.** `Logger.cpp`'s entire print path (`Serial.print`/`println`/`vprintf`) depends on `#include <Arduino.h>` in `Logger.h:4`, which is the *last* consumer pulling the arduino-esp32 component into the link (confirmed: `Pir.h`'s own comment notes it avoids a macro name clash "in translation units that still pull in Arduino.h transitively via Logger.h"; a repo-wide grep for other Arduino/`WiFi.h`/`esp32-hal` usage outside comments turned up nothing — `Mesh.h`/`hw_mac.h`/`Adapter.cpp` all use native `esp_wifi.h` since Phase I Task 3). This is a large *library* (arduino-esp32) kept resident for a handful of `Serial.*` calls a native `uart_write_bytes`/`vsnprintf` implementation (mirroring what `SerialAdapter.cpp` already does for its own UART_NUM_0 traffic since Phase I Task 5, item DD) would replace directly. | Native ESP-IDF `uart_write_bytes` + local `vsnprintf`, replacing the Arduino `Serial` object | **~40 KB flash + several KB DRAM — already measured, not a new estimate.** This exact number comes from the team's own Phase I Task 10 report (`.superpowers/sdd/2026-08-06-phaseI-native-idf/task-10-report.md`, "Concerns" item 1): *"arduino-esp32 kept — decision documented above; matches non-goals. If future task moves Logger to esp_log (with separate UART for text logs), that unlocks the additional ~40 KB flash + several KB DRAM."* Phase I Task 5 deliberately scoped Logger's migration out (design doc item DD: "Logger stays hand-rolled with existing LOG_NONE compile-time fold") to keep that task's diff bounded — this is a real, live, already-quantified follow-up, not a new discovery, and it's the mirror image of the Phase J libsodium lesson: arduino-esp32 is a large framework where only ~5 `Serial.*` call sites are used, so the whole component's link cost is being paid for a sliver of its surface. | **Real candidate, already scoped as a deferred follow-up — not actioned here (read-only audit; out of scope to implement)** |

No `needs-spike` verdicts were assigned: every candidate this sweep found had
enough evidence (either its N is decisively too small for a hash table to
win, or the team already ran the exact build-measure cycle Phase J's
methodology calls for and documented the number) to reach a verdict without
guessing.

## Architecture-boundary reference

`lattice-hub/server/` draws its sharpest boundaries at the *service* level —
`orchestrator/`, `sidecar/`, `dashboard/`, `artist-portal/` are separate Go
modules/npm packages, separate Dockerfiles, separate processes communicating
over HTTP — and within `orchestrator/`, boundary discipline is inconsistent
even there: `nodeauth/` and `eventStore/` are genuinely separate Go packages
with narrow surfaces (registry/persistence/replay only, event-log only), but
`orchestrator/mesh/` itself is one large flat package (~30 files: API
handlers, node registry, zone registry, event broker, masterkey, serial
transport, command store all in one Go namespace/import path) rather than
split by concern. Firmware's `mesh/`, `adapter/`, `hardware/`,
`persistence/`, `app/` (plus `network/`, `crypto/`, `error/`, `logging/`) is
a finer-grained split by directory than hub's `mesh/` package, but the split
is directory-level only — it isn't backed by narrow interfaces the way
`nodeauth`'s package boundary is. The clearest case: `EepromManager.h`
exposes ~30 free functions spanning every persisted concern (keypair, peer
records, mesh key, boot epoch, tx power, node id, dev/master flags) as one
flat `lattice::eeprom` namespace, and `Mesh.cpp`, `Enrollment.cpp`, and
`PeerRegistry.cpp` each `#include` the whole header and call whichever
subset they need directly — there is no `IPersistence`-shaped seam scoped to
"what mesh routing needs" vs. "what enrollment needs," so any of those
consumers could call any persistence function, not just its own slice. That
is muddier than hub's `nodeauth` package (which *is* walled off behind its
own narrow file set), and roughly as muddy as hub's own `mesh/` package —
so nodes' boundary discipline is uneven in the same direction hub's is, not
worse across the board, but the `EepromManager`-as-God-namespace pattern is
firmware's clearest single instance of the "reaches directly into" problem
the brief calls out.
