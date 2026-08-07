# Clean-Code Audit — Findings Ledger

**Status:** Reference document (not an implementation spec). **Phase A complete.**
**Date:** 2026-08-07
**Method:** Phase A of `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md`. File census (Task 1) + 2 parallel subsystem audits (Task 2 mesh, Task 3 non-mesh) + 1 library/architecture scan (Task 4), merged and ranked here (Task 5).
**Purpose:** capture every finding with a stable ID, bucket, and disposition (Phase B / Phase C / new phase / keep-as-is).

**27 findings total** — 7 to Phase B, 12 to Phase C, 8 keep-as-is. **No new phase was
warranted** (see "Phase-bucketing decision" below); the umbrella spec's A–D phase map is
unchanged.

## Bucket assignments

- **Phase B (mesh subsystem, headlined by `Mesh.cpp` decomposition)** — findings **1, 2, 5, 6, 15, 16, 19** (7 items).
- **Phase C (repo-wide sweep)** — findings **3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18** (12 items).
- **Keep-as-is (recorded, no action)** — findings **20, 21, 22, 23, 24, 25, 26, 27** (8 items), plus the file-level entries in the `## Keep-as-is` section.
- **New phase (E, F, …)** — none. See "Phase-bucketing decision".

**Bucket rule used.** Phase B takes every finding whose fix edits a file under
`firmware/main/src/mesh/`; Phase C takes everything else. This is deliberately drawn on
*directory* rather than on "is it literally `Mesh.cpp`", because the umbrella spec has B and C
running in parallel — a rule that sent `Enrollment`/`PeerRegistry`/`ReplayCache` encapsulation
fixes to C while B is restructuring the exact `Mesh.cpp` call sites those fixes rewrite would
guarantee merge conflicts between two concurrent phases. Consequence: **Phase B's scope widens
slightly from "`Mesh.cpp` decomposition" to "mesh subsystem cleanup, headlined by `Mesh.cpp`
decomposition."** The umbrella spec already delegates this ("Boundaries finalized from Phase A
findings"), so no spec edit is needed — but Phase B's implementation plan should adopt the wider
scope explicitly.

Two findings span the boundary, and both are bucketed **Phase C** with a coordination note:

- **Finding 7** (`MacAddress` dead code) deletes two `#include` lines inside `mesh/` alongside a
  non-mesh header. One-line include deletions cannot semantically conflict with Phase B's
  restructuring, and if Phase B moves that code first the includes disappear on their own.
- **Finding 14** (`housekeeping_task_fn`'s inline enrollment-broadcast state machine) removes a
  block from `main.cpp` and lands the extracted helper as a new `Mesh` method. The removal is the
  substance and it is a `main.cpp` edit; the new method is an addition, which does not conflict
  with Phase B's extraction of existing methods. Phase C should add it; Phase B should expect it.

## Full findings table

Ranked in descending order of (maintainability impact) ÷ (effort), the same convention the prior
audit used for flash savings. `Prior ID` is the fragment ID from Tasks 2–4, kept for traceability
back to `task-2-report.md` / `task-3-report.md` / `task-4-report.md`.

| ID | Bucket | Prior ID | File:line | Category | Description | Suggested direction | Effort |
|---|---|---|---|---|---|---|---|
| 1 | Phase B | MESH-1 | `Mesh.h:48`, `Mesh.cpp` (1382 lines) | size/SRP | Confirmed: `Mesh` still does 6 distinct jobs beyond orchestration, matching the umbrella spec's Phase B notes exactly — (1) raw ESP-NOW transport (`setupWiFi:279`, `setupEspNow:312`, `onDataSentCallback:347`, `onDataRecvCallback` (Mesh.h:73), `dataRecvTrampoline:453`, `sendMessage:460`, `broadcastToAllPeers:477`, `sendBroadcast:733`, `drainRecvQueue:390`); (2) message building/dispatch (`buildMessage:212`, `transmitCore:509`, `transmitDispatch:574`, `transmit:582`, `transmitSelfOriginated:590`, `broadcastAdapterData:653`, `sendDownlinkToNode:678`); (3) master-beacon (`broadcastMasterBeacon:603`, `checkMasterTimeout:763`, `processMasterBeacon:781`); (4) downlink routing (`relayDownlink:1042`, `sendRouteReport:1209`, `processRouteReport:1221`, `registerDownlinkPeer:140`); (5) mesh-key persistence (`loadMeshKeyFromEEPROM:620`, `saveMeshKeyToEEPROM:649`); (6) sequence/replay guarding (`nextSeqGuarded:196`, `_checkEpochRollback:55`). All still present, unchanged in shape, on top of `Enrollment`/`PeerRegistry`/`RouteTable`/`NeighborTable`/`E2EKeyStore` orchestration it already delegates to correctly. | No new action here beyond confirming Phase B's existing extraction plan (transport / beacon / downlink-router as composed collaborators, key-persistence folded into `EepromManager`) still matches the current file. See finding 2 for one job the original 6-item list did not capture. | N/A (feeds Phase B directly) |
| 2 | Phase B | MESH-2 | `Mesh.cpp:912-1040` (`processAdapterData`) | size/SRP | A 7th responsibility not enumerated in the umbrella spec's 6-job list: this single 129-line function bundles (a) the uplink-vs-downlink routing decision (relay toward master vs. source-route forward vs. flood fallback), (b) a security gate rejecting unauthenticated sealed frames not addressed to self, (c) E2E AEAD open in both directions (master unseals uplink, node unseals downlink), and (d) config-opcode (`OP_CONFIG_SET`/`OP_NODE_ID_SET`) authorization tied to the opened/unopened state. It doesn't cleanly fit "downlink routing" (4) or "message dispatch" (2) alone — it's routing logic, crypto, and security policy interleaved in one function. | Phase B's implementation plan should explicitly decide where this lands (most likely split: routing decision moves with the downlink-router collaborator; the security-gate + E2E-open + config-authorization logic is security-critical enough that it may be better left as a well-named `Mesh` orchestration method calling into the new collaborators, rather than silently absorbed into either). Flagging so it isn't missed rather than prescribing the exact seam. | M (informs Phase B plan) |
| 3 | Phase C | SYS-1 | `main.cpp:234-579` | size/SRP | `app_main()` (~345 lines) does 15+ distinct jobs in one flat function: NVS partition init/erase-retry, bulk `gpio_config_t` bring-up (3 pin groups), ISR service install, UART driver install + Arduino `initArduino()`/`Serial.begin()`, boot-reason/WDT-loop tracking (delegated to `BootManager::check()` but sequenced here), LED/button/7-seg/EEPROM hardware init — including a ~24-line inlined LED-failure halt loop at `main.cpp:344-367` — dev-mode resolution, default-peer bootstrap, adapter creation+init, mesh init, mesh-drain-task creation + notify-handle wiring, WDT config, provisioning pubkey print, master-role resolution, transmit-fn/callback wiring, housekeeping-task creation, and PM config, all as sequential statements with no intermediate structure. | Extract into a small ordered sequence of boot-phase functions (e.g. `initDrivers()`, `initHardwareOutputs()`, `initSubsystems()`, `spawnTasks()`) called in sequence from `app_main()`, mirroring the "thin orchestrator" direction Phase B applies to `Mesh`. The inlined red-LED-failure halt loop is a legitimate special case (error-signaling itself depends on the LED that just failed) but reads as boot-sequencing logic; a named `haltOnRedLedFailure(Led&, Led&)` helper would make that intent explicit. This is main.cpp's headline SRP finding — `main.cpp` is already named in the umbrella spec's Phase C candidate list. | med |
| 4 | Phase C | SYS-2 | `EepromManager.h:78-154`; `EepromManager.cpp:78-230,234-651` | size/SRP | `EepromManager.cpp` (651 lines) bundles a generic, reusable typed-NVS key/value layer (`nvsGetU8/PutU8/GetU32/PutU32/GetBool/PutBool/GetBytes/PutBytes/Remove/HasKey`, `persistOrEscalate` write-durability policy, `crc16`) with ~12 unrelated persisted-domain concerns each exposed as its own load/save pair in one flat `lattice::eeprom` namespace: device role (master/dev flags), mesh security (mesh key), peer directory (per-record + list-level peer accessors), adapter config, boot diagnostics (reboot count/reason), device identity (long-term keypair + CRC, node ID), enrollment status, anti-replay state (boot epoch), trust anchors (primary/secondary known-master MAC), and RF tuning (TX power preset). 41 functions are declared in the header. The KV-primitive layer is already correctly private (anonymous namespace) and file-static `_state` is correctly encapsulated (only escapes via `debugStateForTest()`, gated `#ifdef UNIT_TEST`) — this is a size/responsibility-count finding, not an encapsulation one. See also the Architecture-boundary reference section: this is firmware's clearest single instance of the "any consumer can reach any persistence function" problem. | Matches the umbrella spec's Phase C candidate list (`EepromManager.cpp` already named there). Direction: keep the generic typed-KV wrapper + durability policy as one small reusable layer, and group the 41 public domain accessors by concern (nested namespaces, e.g. `lattice::eeprom::identity::`/`::security::`/`::diagnostics::`, or split files) rather than one flat namespace — exact boundary is a Phase C implementation-plan decision, not this audit's. | med |
| 5 | Phase B | MESH-4 | `PeerRegistry.h:32-33` (`peerMacs`, `peerCount`, both `public`); reached into at `Mesh.cpp:337-338,478,482-485,1048-1051,1111,1113-1114,1140` and `Mesh.h:402-403` | encapsulation | `PeerRegistry` exposes its backing array and count as public fields (comment: "Peer list management (no heap alloc)"). `Mesh` iterates/indexes them directly in 5 places (`setupEspNow`'s peer-registration loop, `broadcastToAllPeers`, `relayDownlink`, `addPeer`, and a bare `peers.peerCount` read at `Mesh.cpp:1140`) instead of going through `PeerRegistry`'s own `find`/`append`/etc. API — even though `PeerRegistry` already has a proper method surface for everything else (`find`, `isPeerInRange`, `updateLastSeen`, `addAndPersist`, `removeAndPersist`). `getPeerList()`/`getPeerCount()` on `Mesh` (Mesh.h:402-403) also leak the raw pointer straight through to any external caller. | Make `peerMacs`/`peerCount` private (no heap/behavior change — same fixed array, same zero-alloc storage). Add a const iteration surface (e.g. `size_t count() const` — already effectively `getPeerCount()` — plus `const PeerInfo& at(size_t i) const` or `begin()/end()` over the live prefix) so both `Mesh`'s internal loops and `getPeerList()`'s external contract go through `PeerRegistry`'s own API instead of touching the array/count fields directly. | S |
| 6 | Phase B | MESH-3 | `Enrollment.h:22-25` (`hasMasterMac`, `knownMasterMac`, `hasMasterMacSecondary`, `knownMasterMacSecondary`, all `public`); reached into at `Mesh.cpp:304,494,810-828,840-841,917,1020-1024` | encapsulation | `Enrollment`'s TOFU master-identity state is public data, not behavior. `Mesh::processMasterBeacon` (and 4 other sites) directly `memcpy`s into `knownMasterMac`/`knownMasterMacSecondary` and assigns `hasMasterMac`/`hasMasterMacSecondary`, then separately calls `lattice::eeprom::saveKnownMasterMac(...)` — reimplementing, at 3 call sites in `Mesh.cpp` (817-819, 826-828, 840-841), the exact "memcpy + set flag + EEPROM-save" idiom `Enrollment::processJoinAck` already implements privately at 2 sites in its own file (Enrollment.cpp:161-164, 185-188). Grep-verified: no other file outside `Mesh.cpp` reaches into these fields. | Make the 4 fields `private`. Add `Enrollment::learnMasterMac(const uint8_t* mac)` / `learnSecondaryMasterMac(const uint8_t* mac)` methods owning the memcpy+flag+persist triple; replace all 5 call sites (3 in `Mesh.cpp`, folding the 2 already-private ones in `Enrollment.cpp` through the same method). Removes both the encapsulation break and the duplicated persistence idiom in one move. | S-M |
| 7 | Phase C | SYS-8 | `network/MacAddress.h:12-44`; dead includes at `mesh/Mesh.cpp:2`, `mesh/PeerRegistry.h:5`; stale comment at `mesh/Mesh.cpp:31` | size/SRP (dead code) | The file census already flagged this file as a self-documented superseded-idiom smell (`MacEq.h`'s comment: `MacAddress` equality is "strictly worse" than `mac::eq()`; `toString()`'s own comment: "No callers exist yet"). This audit grep-confirmed it further: zero real instantiations of `lattice::utils::MacAddress` exist anywhere in `firmware/main/src` today — the only two `#include "src/network/MacAddress.h"` sites outside the file itself (`Mesh.cpp:2`, `PeerRegistry.h:5`) are dead includes, left over from before the Phase G item N/Q MAC-idiom consolidation onto raw `uint8_t[6]` + `lattice::mac::eq()`. `Mesh.cpp:31`'s comment ("no longer need macEquals helper – use MacAddress equality directly") is itself stale for the same reason. **Cross-task overlap resolved:** Task 3 raised the deletion and deferred the mesh-side half to the mesh audit; Task 2's read of the mesh subsystem surfaced no latent need for a MAC value type (every mesh table stores raw `uint8_t[6]` and compares via `lattice::mac::eq`, grep-confirmed across `Mesh.cpp`, `PeerRegistry`, `NeighborTable`, `RouteTable`, `E2EKeyStore`, `ReplayCache`). The two halves are therefore one finding, not two. | Delete the two dead includes, delete the stale `Mesh.cpp:31` comment, and delete the `MacAddress` struct and its header entirely — the mesh audit's answer to Task 3's open question is "no reason to keep it." Coordination: the two include deletions are inside `mesh/`, so if Phase B lands first they may already be gone; the `network/MacAddress.h` deletion must land after both includers are clean. | trivial |
| 8 | Phase C | SYS-9 | `Error.h:62-64` | size/SRP | Two parallel error-reporting APIs exist: the digit-based `err::fail(ErrorTypeDigit, ModuleDigit, uint8_t, msg)` (32 call sites across the tree) and a "legacy" `err::fail(utils::ErrorType, msg)` overload that converts via `toDigit()` to a generic sub-code 0 (2 call sites total, both in `main.cpp` — config/reset button init failures). The legacy path is a thin, lossier wrapper (always `ModuleDigit::CORE`, sub-code 0) that's now used at only 2 of 34 total call sites. | Migrate the 2 remaining `main.cpp` call sites to the digit-based `fail(ErrorTypeDigit, ModuleDigit, sub, msg)` form (there's already a `ModuleDigit::HW`/similar precedent used elsewhere for hardware-init failures), then delete the legacy `fail(utils::ErrorType, ...)`/`fatal(utils::ErrorType, ...)` overloads and `toDigit()`. Small flash win, and one error-reporting idiom instead of two. | trivial |
| 9 | Phase C | SYS-4 | `Button.cpp:11-21` vs `Pir.cpp:9-14`, `Led.cpp:28-38` | inheritance | Asymmetric use of the existing `GpioInput`/`GpioOutput` base: `Pir::init()` correctly delegates to `GpioInput::init()` and `Led::init()` correctly delegates to `GpioOutput::init()`, but `Button::init()` re-implements the identical pin-validate-then-`_initialized=true` logic instead of calling the inherited `GpioInput::init()` it could reuse — a real, evidenced answer to the umbrella spec's "is there duplication a shared base would remove" question: yes, the base already removes it for 2 of 3 siblings, `Button` just doesn't use it. | Delete `Button::init()`'s body and either call `GpioInput::init()` explicitly (matching `Pir`'s pattern) or remove the override entirely and let `Button` inherit `GpioInput::init()` directly — it adds nothing beyond the base behavior today. | trivial |
| 10 | Phase C | SYS-6 | `Adapter.cpp:240-242` | size/SRP (dead code) | `Adapter::init()` is declared pure virtual (`= 0`) in `Adapter.h:50` yet has an out-of-line body (`return true;`) in `Adapter.cpp`. A pure-virtual body is only reachable via an explicit `Base::method()` qualified call from a derived override — grep-confirmed neither `PirAdapter::init()` nor `SerialAdapter::init()` makes that call (unlike `Pir`/`Led`'s pattern of calling into `GpioInput`/`GpioOutput`, see finding 9), so this body is dead code. | Delete the body; leave `init()` declared pure-virtual-only in the header (no definition needed). If the intent was for subclasses to reuse shared validation the way `Pir`/`Led` do for their base, that reuse doesn't exist today and would need to be added deliberately, not assumed. | trivial |
| 11 | Phase C | SYS-7 | `Adapter.h:27` | size/SRP (dead code) | `adapter_types::LED_ADAPTER = 3` is unreachable: `AdapterFactory::createAdapter()`'s switch only handles `PIR_ADAPTER`/`SERIAL_ADAPTER` and treats anything else (including `LED_ADAPTER`) as an error via the `default:` case. `AdapterFactory.h:13`'s own comment documents that the LED stub and its default pin were removed in Phase G Task 2 — this enum value is the one piece of that removal left behind. | Delete the `LED_ADAPTER` enumerator (and confirm no wire-protocol dependency on its numeric value `3` before removing — `lattice-protocol`'s `adapter_types` mirror, if any, would need the same trim). | trivial |
| 12 | Phase C | SYS-5 | `GpioInput.h:12`, `GpioOutput.h:12` | inheritance | `virtual ~GpioInput() = default;` / `virtual ~GpioOutput() = default;` remain the only virtual members of either class (post-Phase-G item I already dropped `virtual` from `init()` with the documented reasoning "never dispatched through a `GpioInput*`/`GpioOutput*` base pointer" — item I's own comment scoped that fix to `init()` only, leaving the destructor "out of scope"). Grep-confirmed zero occurrences of `GpioInput*`, `GpioOutput*`, or a `unique_ptr`/`delete` through either, anywhere in the tree — every user (`Button`, `Pir`, `Led`) is always held/destroyed by concrete type. Because C++ makes a derived dtor implicitly virtual once its base's is, this vtable is also carried by `Button`, `Pir`, and `Led` (`SevenSegDisplay`, which doesn't inherit `GpioOutput`, is unaffected). Item I's own reasoning still holds and now extends to this second vestige. | Drop `virtual` from both destructors (`~GpioInput() = default;` / `~GpioOutput() = default;`), completing item I's cleanup. Removes one vtable pointer per `Button`/`Pir`/`Led`/`GpioInput`/`GpioOutput` instance plus the vtable's flash footprint. | trivial |
| 13 | Phase C | SYS-10 | `app/ButtonHandler.h:44-122` | size/SRP | `tickConfig()` and `tickReset()` each hand-roll their own "press-and-hold for `HOLD_MS`" detection skeleton (own `static bool wasPressed; static uint64_t holdStart;`, same `if (btn.isPressed()) { if (!wasPressed) {...} else if (now - holdStart >= HOLD_MS) {...} } else { wasPressed = false; }` shape) — `tickReset` additionally layers a second "confirm within 3s" phase on top. This is the duplication the file census's judgment call flagged this file for (highest fan-in file under the size threshold). | Extract a shared `static bool detectHold(Button&, uint64_t holdMs, bool& wasPressed, uint64_t& holdStart)` helper (or a tiny local struct holding the two static-equivalent fields) used by both `tickConfig`/`tickReset`, leaving only the differing post-hold action (role toggle vs. arm/confirm wipe) in each. | low |
| 14 | Phase C | SYS-3 | `main.cpp:118-192` | size/SRP | `housekeeping_task_fn` (the old Arduino `loop()` body, ~75 lines) mixes 8 concerns in one function: LED pulse-pump, `err_core::tick()`, one-shot startup blink, `mesh.loop()`/`checkMasterTimeout()` delegation, 7-seg `DisplayManager::tick()` delegation, an inline enrollment-broadcast state machine (its own `static uint64_t lastEnrollmentBroadcast`, 10s interval, `sendEnrollmentRequest()` call — genuine logic, not delegation, at `main.cpp:168-178`), WDT reset, and adapter/`ButtonHandler::tick()` delegation. Most of this is thin delegation (acceptable for a task-loop body), but the enrollment-broadcast block is real state-machine logic that doesn't belong at this altitude. | Extract the enrollment-broadcast block into a small helper (e.g. `Mesh::tickEnrollmentBroadcast()` alongside `Mesh`'s existing `sendEnrollmentRequest()`/`isEnrolled()`, since `Mesh` already owns the state this decision is based on) so `housekeeping_task_fn` stays pure delegation. Note the helper itself lands in `mesh/`, so sequence it with Phase B. | low |
| 15 | Phase B | MESH-5 | `ReplayCache.h:13-32` (`struct`, all fields public); mutated directly at `Mesh.cpp:204-206,228,246,1177` (`bootEpoch`) and `Mesh.cpp:884-893` (`lastRelayedEpoch`/`lastRelayedSeqNum`) | encapsulation | `ReplayCache` is a bare `struct` bundling two unrelated concerns under one name: (a) the actual per-origin replay-detection table (`cache[]`, `isReplay()` — genuinely "a replay cache") and (b) this node's own outbound bookkeeping — `txSeqNum`/`nextSeq()` (this node's outbound sequence counter) and `lastRelayedEpoch`/`lastRelayedSeqNum` (last-relayed-beacon dedup, unrelated to incoming replay detection). All four of group (b)'s fields are public and mutated directly by `Mesh.cpp` (`replay.bootEpoch = epoch;` at line 206, `replay.lastRelayedEpoch = msg.epoch_num;` / `replay.lastRelayedSeqNum = msg.seq_num;` at 892-893) rather than through a method — unlike `cache[]` itself, which stays properly encapsulated (grep-verified: no external access to `replay.cache`). | Split the two concerns: keep `ReplayCache` narrowly scoped to incoming-message replay detection (`cache[]`/`isReplay()`); move `bootEpoch`/`txSeqNum`/`lastRelayedEpoch`/`lastRelayedSeqNum` — this node's own outbound state — onto `Mesh` directly (they already conceptually belong to Mesh's sequence/replay-guarding job, job 6 in finding 1) or a small dedicated struct with real mutator methods (`bumpEpoch()`, `markRelayed(epoch, seq)`, `wasRelayedBefore(epoch, seq)`) instead of raw public fields. | M |
| 16 | Phase B | MESH-8 | `Enrollment.h`/`Enrollment.cpp` (89 + 224 = 313 lines) | size/SRP | `Enrollment` does ~5 distinct jobs: (a) keypair lifecycle (`init:42` load-or-generate, EEPROM persist); (b) TOFU master-identity state (see finding 6); (c) enrollment-protocol messaging (`sendRequest:66`, `processRequest:89`); (d) JOIN_ACK handling (`processJoinAck:107-190`, 84 lines alone — fingerprint check, pubkey-pin verification, TOFU origin gate, peer registration, secondary-master registration, all in one function); (e) a generic bounded pending-relay FIFO (`PendingRelay` struct at Enrollment.h:67-71, `enqueuePendingRelay:95`, `drainPendingRelay:210`, `setPendingRelay:206`) that is a queueing-utility concern, not enrollment-domain logic — it happens to hold enrollment-request payloads, but the FIFO mechanics themselves are identical in kind to `Mesh::recvQueue`'s ring buffer. | At minimum, extract the pending-relay FIFO into its own small queue type (mirrors the ring-buffer pattern `Mesh.recvQueue`/Enrollment's own ring buffer already use) so `Enrollment` reads as "enrollment protocol state machine" without also being "a small SPSC queue implementation". Combine with finding 6's TOFU-state method extraction — same files, one pass. `processJoinAck`'s internal density (b/c/d/e above are all *within* one function once inlined) is defensible as one atomic security check sequence — not recommending it be split further without a stronger reason than line count. | M |
| 17 | Phase C | LIB-05 | `logging/Logger.h:4` (`#include <Arduino.h>`), `logging/Logger.cpp` (print path), `main.cpp` (`initArduino()`/`Serial.begin()`) | library | **Inverse library case — the only live library candidate the whole sweep found.** `Logger.cpp`'s entire print path (`Serial.print`/`println`/`vprintf`) depends on `#include <Arduino.h>` in `Logger.h:4`, which is the *last* consumer pulling the arduino-esp32 component into the link (confirmed: `Pir.h`'s own comment notes it avoids a macro name clash in translation units that still pull in Arduino.h transitively via `Logger.h`; a repo-wide grep for other Arduino/`WiFi.h`/`esp32-hal` usage outside comments turned up nothing — `Mesh.h`/`hw_mac.h`/`Adapter.cpp` all use native `esp_wifi.h` since Phase I Task 3). A large framework is kept resident for a handful of `Serial.*` calls that a native `uart_write_bytes`/`vsnprintf` implementation (mirroring what `SerialAdapter.cpp` already does for its own UART_NUM_0 traffic since Phase I Task 5, item DD) would replace directly. **Delta is measured, not estimated: ~40 KB flash + several KB DRAM**, from the team's own Phase I Task 10 report (`.superpowers/sdd/2026-08-06-phaseI-native-idf/task-10-report.md`, "Concerns" item 1). This is the mirror image of the Phase J libsodium lesson: a large framework whose whole link cost is paid for a sliver of its surface. | Migrate `Logger` to native ESP-IDF `uart_write_bytes` + local `vsnprintf` and drop the arduino-esp32 component dependency. **Must ship as its own standalone PR inside Phase C, with its own before/after size measurement reported separately** — a −40 KB delta folded into the sweep's aggregate would defeat the umbrella spec's "a readability change must not silently regress the flash/RAM budget" CI gate for every other Phase C item. It is also legitimate to *drop* this from the effort: it is an efficiency win, not a readability one, and it remains tracked as a Phase I follow-up either way. See "Phase-bucketing decision" for why it did not get its own phase. | med |
| 18 | Phase C | SYS-11 | `main.cpp:337,396` | size/SRP | `lattice::eeprom::init()` is called twice: once unchecked at `main.cpp:337` (needed early so `BootManager::check()`'s reboot-reason/-count calls aren't no-ops) and once checked-with-fatal-escalation at `main.cpp:396` ("Initialize EEPROM Manager"). `eeprom::init()` is idempotent (early-returns `true` if `_state.isInitialized`), so this is behaviorally safe, but reads as if EEPROM might be initialized twice, and the first call's failure is silently swallowed (only the second call's failure escalates to fatal) — the intent (early unchecked probe vs. later checked real init) isn't obvious from the call sites alone. | Add a one-line comment at `main.cpp:337` noting the second checked call at `:396` is the authoritative one (or restructure so there's exactly one init call, with `BootManager::check()` moved after it) — minor clarity fix, not a functional one. Fold into finding 3's `app_main()` restructuring rather than doing standalone. | trivial |
| 19 | Phase B | MESH-7 | `MeshCrypto.h:16` (`registerPeerWithEspNow`), `MeshCrypto.h:38` (`generateKeypair`) | size/SRP | Minor/optional: `MeshCrypto.h` (47 lines) bundles two unrelated concerns under one name that oversells both — ESP-NOW peer registration *without* link-layer encryption (a transport/peering concern) sits next to X25519 keypair generation (a crypto concern). Small enough (2 functions) that splitting doesn't clearly earn its keep on its own, but the file name suggests one coherent "mesh crypto" surface when it's actually "peer registration helper" + "keygen wrapper". | **Opportunistic only — do not do standalone.** Phase B is already extracting a transport collaborator out of `Mesh`; when it does, move `registerPeerWithEspNow` next to that collaborator (it's peering, not crypto) and leave `generateKeypair` where it is, or fold it into `E2ECrypto.h`, which already owns `computeSharedSecret`/`deriveE2EKeys`. If Phase B's seam doesn't naturally absorb it, drop the finding. | XS (opportunistic) |
| 20 | Keep-as-is | SYS-12 | `adapter/serial/SerialFraming.h:17-42` | size/SRP | `SerialFraming` bundles two distinct jobs in one small class: nanopb protobuf marshalling (`encode`/`decode`, static, stateless) and a byte-stream length-prefixed frame-reassembly state machine (`injectByte`, instance state: `frameState`/`frameLength`/`frameIndex`/`payloadBuffer`). Both serve "get a `mesh_message` on/off the wire," so this is a low-confidence, borderline finding — the file is small (209 lines) and the two jobs are related, not clashing. | **No action.** Recorded as considered-and-declined. Revival trigger: if `adapter/serial/` grows further, split into a stateless `SerialFrameCodec` (the two static methods) and a `SerialFrameReader` (the `injectByte` state machine). Not worth doing in isolation. | low (declined) |
| 21 | Keep-as-is | SYS-13 | `hardware/output/SevenSegDisplay.cpp:133-197` | size/SRP | `SevenSegDisplay` (200 lines, file-census-flagged) mixes the low-level TM1637 bit-bang protocol pump (`start`/`stop`/`writeByte`, timing-sensitive) with digit-encoding/formatting logic (`FONT` table, `encodeDigit`, `showInternal`'s value-to-4-digit-segments formatting). Both concerns exist to serve "drive one physical 7-segment peripheral," and the class correctly does not inherit `GpioOutput` (it owns two pins in a bit-banged protocol, not a single digital I/O — composition via `GpioOutput::isValidOutputPin()` static reuse is the right call here, confirmed no finding on that front). Low-confidence, borderline finding — this is closer to inherent complexity of one bit-banged driver than an SRP violation. | **No action.** Recorded as considered-and-declined. Revival trigger: if the display logic needs to be reused or unit-tested independent of hardware, extract `encodeDigit`/`FONT`/the digit-splitting portion of `showInternal` into a free-function formatter (`uint8_t[4] formatSevenSeg(int value, bool leadingZeros, bool withDP)`) separate from the TM1637 protocol pump. | low (declined) |
| 22 | Keep-as-is | MESH-9 | `PeerRegistry.h`/`PeerRegistry.cpp` (57 + 184 = 241 lines) | size/SRP | Confirmed 2 distinct jobs: in-memory peer-list management (`find`, `append`, `remove`, `isPeerInRange`, `updateLastSeen`) vs. EEPROM persistence (`loadFromEEPROM`, `saveToEEPROM`, `addAndPersist`, `removeAndPersist`, per-key `peer0`..`peer9` blob I/O). Reasonably cohesive pairing — the persistence directly serializes the list it manages, and `PeerRegistry` is the only one of the 5 MAC-keyed tables that owns real persistence (the other 4 — `NeighborTable`/`RouteTable`/`E2EKeyStore`/`ReplayCache` — are RAM-only by design), so this isn't an accident of scope creep, it's inherent to what `PeerRegistry` is. | **No action.** The current split has no duplication or confusion to fix on its own. Revival trigger: only if Phase C decides to apply a uniform "persistence adapter" pattern across every stateful class for consistency's sake. Listed for completeness per the audit's per-class breakdown requirement, not as a recommended action. (Note: finding 5 does change `PeerRegistry.h`'s field visibility — that is a separate, accepted finding.) | XS (declined) |
| 23 | Keep-as-is | MESH-6 | `network/mac_table.h` (used by `NeighborTable.h`, `RouteTable.h`, `E2EKeyStore.h`, `ReplayCache.h`, `PeerRegistry.cpp`) | inheritance | Explicitly checked per this effort's "inheritance sparingly" constraint: should the 5 classes that already dedup their "linear-scan-by-MAC" / "evict-oldest-by-timestamp" skeleton via `lattice::mac_table::find`/`evict_oldest_by_ts` free functions (Phase H2 item Y) instead share a base class? **No — free functions are already the correct call, not a missed inheritance opportunity.** Reasoning: (1) `mac_table.h`'s own header comment already rejected templates for this, to avoid a template instantiated per entry type bloating flash — a base class faces the identical tradeoff: a non-template base can't touch each type's specific `Entry` layout, and a template/CRTP base reintroduces exactly that per-instantiation bloat. (2) There is no real polymorphic call site: grep-verified, every one of the 5 classes is used by its own concrete type name throughout `Mesh` — nothing holds a `TableBase*`/reference and dispatches virtually across them, unlike the `Adapter`/`PirAdapter`/`SerialAdapter` precedent the umbrella spec cites, where callers genuinely hold an `Adapter*` and need runtime substitution. (3) The 5 classes' real APIs diverge well beyond the shared find/evict sliver (`NeighborTable::selectNextHop`/`minFreshDistance`, `RouteTable::lookup`/`record`, `ReplayCache::isReplay`, `E2EKeyStore::getKeys`/`setCapacity`, `PeerRegistry::addAndPersist`/EEPROM) — a base class would capture only the already-factored-out sliver while adding a vtable (RAM: vptr/instance; flash: virtual dispatch) for zero additional behavioral reuse over what the static free-function calls already give for free. | **No action.** Documented as "considered, rejected" so a future pass doesn't re-propose it without re-deriving this reasoning. | N/A (negative finding) |
| 24 | Keep-as-is | LIB-01 | `RouteTable::entries[16]`, `NeighborTable::entries[8]`, `PeerRegistry::peerMacs[MAX_PEERS]`, `ReplayCache::cache[12]`, `E2EKeyStore::entries[≤10]` | library | Five independent fixed-size linear-scan-by-MAC arrays, already deduped onto a shared non-templated helper (`network/mac_table.h`, Phase H2 item Y). Candidate: a hash table / hash-map component (no ESP-IDF-native small hash-table library exists; would mean `std::unordered_map` or a third-party header-only hash map). **No credible delta — negative expected value, no spike needed.** All five tables cap at N ∈ {8, 10, 12, 16, MAX_PEERS}; a linear scan over ≤16 six-byte MAC compares is a handful of cycles and zero extra code. `std::unordered_map` pulls in dynamic allocation, a hash function, bucket-array bookkeeping, and (per-instantiation, since these are 5 distinct entry types) that cost multiplies by five unless further genericized — the same per-entry-type bloat `mac_table.h` deliberately avoided templates to prevent. A hash table is worse on both axes (flash and RAM) at this scale. | **Reject.** No action. | N/A |
| 25 | Keep-as-is | LIB-02 | `persistence/EepromManager.cpp:30-48` (`crc16`) | library | Hand-rolled non-reflected CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`), used only to self-check a persisted 64-byte keypair (write vs. read, no external interop). Candidate: `esp_rom_crc16_le`/`esp_rom_crc16_be` (ESP-IDF ROM CRC — already resident in every ESP32 binary at effectively zero marginal flash cost). **Already evaluated — no new estimate needed.** Phase I Task 4 (item UU) considered this exact swap and rejected it: neither ROM variant is a bit-exact match for this specific non-reflected CRC-CCITT-FALSE without an unverified pre/post-invert transform (`~crc16_be(~init, ...)`), and since the value is self-referential (round-trips through this device's own NVS only) the correctness risk of an unverified transform outweighs the near-zero flash win available. See `EepromManager.cpp:31-40` for the team's own reasoning, still accurate — this sweep found no new evidence to revisit it. | **Reject (re-confirmed prior finding).** No action. | N/A |
| 26 | Keep-as-is | LIB-03 | `adapter/pir/PirAdapter.h:38` (`PirState`), `adapter/serial/SerialFraming.h:36` (`FrameState`), dispatched via `switch` in `SerialFraming.cpp:162-204` | library | Two 3-state hand-rolled FSMs (`enum class` + `switch`). Candidate: a generic FSM framework (tinyfsm, Boost.SML — both header-only, template-heavy; neither ESP-IDF-native). **Negative on its face — no spike needed.** `enum class` + `switch` is already the zero-overhead idiomatic embedded C++ pattern: no vtable, no function-pointer table, no per-state object. A template-based FSM library adds an entire templated dispatch mechanism (and its own instantiation-per-state-machine flash cost, the same bloat pattern `mac_table.h` was written to avoid) to formalize something a 3-case switch already expresses correctly and minimally. Also: `PirState` was itself the *product* of a Phase G audit collapse (item K, two overlapping bools → one enum) — the pattern here is the target state of a prior simplification, not a leftover needing one. | **Reject.** No action. | N/A |
| 27 | Keep-as-is | LIB-04 | ESP-NOW receive queue (`Mesh.h`/`Mesh.cpp`), enrollment relay queue (`Enrollment.h`/`.cpp`) | library | Grepping for "ring" surfaces these, but they are **not currently hand-rolled** — Phase I Task 8 (item OO) already replaced a hand-rolled head/tail/count SPSC array with FreeRTOS's native `xRingbufferCreateStatic`/`RINGBUF_TYPE_NOSPLIT` for both queues. The grep's "ring" hit is residual naming/comments, not hand-rolled code. Recorded explicitly so a future sweep doesn't re-list it as an open candidate. | **N/A (already resolved).** No action. | N/A |

No `needs-spike` verdicts were assigned anywhere in the library scan: every candidate had enough
evidence (either its N is decisively too small for a library to win, or the team already ran the
exact build-measure cycle Phase J's methodology calls for and documented the number) to reach a
verdict without guessing.

## Phase-bucketing decision: no new phase

The umbrella spec's "Phase list is not closed" clause was evaluated explicitly. **No finding
clears the bar `Mesh.cpp` cleared for Phase B, so no new phase (E, F, …) is added and the
umbrella spec is unchanged.** Three findings were seriously considered and rejected:

- **Finding 3 (`main.cpp`, 579 lines / one 345-line function doing 15+ jobs)** — the second-largest
  God-function in the tree, and genuinely PR-sized on its own. Rejected because the umbrella spec
  already names `main.cpp` in Phase C's candidate list by name ("candidates so far:
  `EepromManager.cpp` 651 lines, `main.cpp` 579 lines"). Promoting a file the spec pre-assigned to
  C would contradict the spec without the audit having found it *bigger* than the spec assumed —
  it found it exactly as big as assumed.
- **Finding 4 (`EepromManager`, 651 + 156 lines, 41 public functions across ~12 domains)** — same
  reasoning, plus it is the largest-blast-radius item in the audit (every consumer file calls into
  it). Rejected for the same reason: pre-assigned to Phase C by name. Its size is roughly half
  `Mesh.cpp`'s, and its work is mechanical (namespace grouping / file split) rather than
  structural collaborator extraction.
- **Finding 17 (drop arduino-esp32 by migrating `Logger` to native UART)** — the single largest
  quantified win in the audit (~40 KB flash + several KB DRAM, already measured). Rejected on the
  bar as written, which is a *size* bar: the code change is ~200 lines of `Logger` plus a handful
  of `main.cpp` lines and a component-dependency removal — far below `Mesh.cpp`'s scale, and below
  findings 3 and 4's. What *is* unusual about it is its **kind**, not its size: a different risk
  profile (build graph + logging transport, not readability) and a size delta large enough to
  swamp Phase C's CI size-delta gate. That is handled by the standalone-PR + separate-measurement
  constraint recorded on the row itself, which is sufficient without a phase of its own.

**Sizing precedent supports this.** The prior effort's audit-driven phases carried 17 (G), 10 (H),
and 9 (I) items each, routinely including medium-scope refactors. Phase C at 12 items — two
medium (3, 4), one medium-isolated (17), two low (13, 14), and seven trivial — is squarely in
line with this project's own phase sizing. It is a sweep, not a dumping ground.

If the maintainer disagrees on finding 17, promoting it is a one-row edit to the umbrella spec's
phase map plus one line in the dependency graph (gated by A, parallel to B/C, with D then waiting
on it too). Nothing else in this ledger depends on that choice.

## Architecture-boundary reference

`lattice-hub/server/` draws its sharpest boundaries at the *service* level — `orchestrator/`,
`sidecar/`, `dashboard/`, `artist-portal/` are separate Go modules/npm packages, separate
Dockerfiles, separate processes communicating over HTTP — and within `orchestrator/`, boundary
discipline is inconsistent even there: `nodeauth/` and `eventStore/` are genuinely separate Go
packages with narrow surfaces (registry/persistence/replay only, event-log only), but
`orchestrator/mesh/` itself is one large flat package (~41 files: API handlers, node registry,
zone registry, event broker, masterkey, serial transport, command store all in one Go
namespace/import path) rather than split by concern.

Firmware's `mesh/`, `adapter/`, `hardware/`, `persistence/`, `app/` (plus `network/`, `crypto/`,
`error/`, `logging/`) is a finer-grained split by directory than hub's `mesh/` package, but the
split is directory-level only — it isn't backed by narrow interfaces the way `nodeauth`'s package
boundary is.

The clearest case: `EepromManager.h` declares **41 free functions** spanning every persisted
concern (keypair, peer records, mesh key, boot epoch, tx power, node id, dev/master flags) as one
flat `lattice::eeprom` namespace, and `Mesh.cpp`, `Enrollment.cpp`, and `PeerRegistry.cpp` each
`#include` the whole header and call whichever subset they need directly — the `mesh/` directory
alone reaches **18 distinct `eeprom::` functions across 22 call sites**. There is no
`IPersistence`-shaped seam scoped to "what mesh routing needs" vs. "what enrollment needs," so any
of those consumers could call any persistence function, not just its own slice.

That is muddier than hub's `nodeauth` package (which *is* walled off behind its own narrow file
set), and roughly as muddy as hub's own `mesh/` package — so nodes' boundary discipline is uneven
in the same direction hub's is, not worse across the board. But the `EepromManager`-as-God-
namespace pattern is firmware's clearest single instance of the "reaches directly into" problem,
and it is what finding 4 asks Phase C to address. This section is a pattern reference only — no
code reuse, hub is Go and nodes is C++.

## Keep-as-is

### Findings recorded but not actioned

Findings **20–27** in the table above. Summary of why each stays:

- **20 — `SerialFraming` two jobs.** Both serve one wire concern; file is small. Declined pending
  growth in `adapter/serial/`.
- **21 — `SevenSegDisplay` protocol vs. formatting.** Inherent complexity of one bit-banged
  driver, not an SRP violation. Declined pending a need to unit-test the formatter off-hardware.
- **22 — `PeerRegistry` list vs. persistence.** The persistence directly serializes the list it
  manages; it is the only MAC-keyed table with real persistence, by design, not by scope creep.
- **23 — Base class for the 5 MAC-keyed tables.** Considered and rejected: no polymorphic call
  site exists, and a base would add a vtable for a sliver the free functions already dedup.
- **24 — Hash map for the MAC tables.** N ≤ 16; a hash map is worse on flash *and* RAM.
- **25 — `esp_rom_crc16` for the keypair checksum.** Not bit-exact without an unverified transform;
  correctness risk beats a near-zero flash win. Re-confirms Phase I item UU.
- **26 — FSM framework for the two 3-state machines.** `enum class` + `switch` is already the
  zero-overhead form; `PirState` is itself the product of a prior simplification.
- **27 — Ring-buffer library.** Already on FreeRTOS `xRingbufferCreateStatic` since Phase I item OO.

### Census-flagged files with zero findings

Every file the Task 1 census marked `investigate` that no finding above points at. Each was read
and judged by Task 2 or Task 3 and found already correct for the embedded constraints — the flag
was a size trigger, not a defect.

- **`adapter/serial/SerialAdapter.cpp` (310 lines)** — one cohesive job: the `Adapter` subclass for
  the UART-attached server link (`init`/`loop`/`onMeshDataImpl`/`relayEnrollmentToServer`/
  `handleCompleteFrame`). Its bulk is a single inbound-opcode dispatch in `handleCompleteFrame`,
  which is what the adapter contract exists to do; health-frame building was already lifted into
  the shared base by Phase H2 item W. Flagged on line count only.
- **`mesh/NeighborTable.h` (203 lines)** — one job: a bounded MAC-keyed table of routing
  candidates. All data members `private`, only behavior methods `public`; grep-verified no external
  file reaches into `entries`/`capacity_`/`nextSlot`. Flagged on line count only.
- **`project_config.h` (185 lines)** — one job: the firmware's single compile-time constant
  surface, organized into 9 numbered sections and dominated by explanatory comments and
  `constexpr` declarations. Splitting it would scatter the tuning knobs it exists to centralise.
- **`hardware/output/Led.cpp` (162 lines)** — one class, one peripheral: a single digital-output
  LED, correctly delegating pin validation and init to `GpioOutput` (the pattern finding 9 says
  `Button` should copy). The extra length is the non-blocking `pulse`/`update`/`isBusy` timer and
  the static system-error-LED hook — LED behavior, not a second concern.
- **`crypto/Crypto.h` (159 lines)** — deliberately the firmware's single crypto-primitive surface
  and, by design, the only file that includes mbedtls headers (Phase J: "swap the backend again and
  only this file changes"). Its length is the load-bearing big-endian-key-convention documentation
  plus 8 thin inline primitive wrappers. Splitting it would reintroduce exactly the multi-file
  mbedtls sprawl Phase J removed.

### Confirmed clean (checked, no finding, mesh subsystem)

- **`RouteTable.h`, `E2EKeyStore.h`** (and `NeighborTable.h`, above) — each does exactly one job
  (bounded MAC-keyed table for its own domain: master-side route paths, derived E2E keys), all data
  members `private`, only behavior methods `public`. Grep-verified: no external file reaches into
  any internal field.
- **`E2ECrypto.h`, `RouteMac.h`, `CompactMessage.{h,cpp}`, `broadcast_mac.h`** — stateless
  free-function namespaces (`E2ECrypto`, `RouteMac`), a POD struct + pure converter functions
  (`CompactMessage`), or a single `constexpr` (`broadcast_mac.h`). Correctly *not* classes: no state
  to encapsulate, no polymorphic dispatch need.
- **`Adapter`/`PirAdapter`/`SerialAdapter` hierarchy** — re-verified rather than assumed from the
  umbrella spec's note that this is the reference example of "real" inheritance. `main.cpp` holds
  `std::unique_ptr<Adapter>` and dispatches `init()`/`loop()`/`onMeshData()` through it: genuine
  base-pointer polymorphism. Members are `protected`/`private` with zero external reach-in. The one
  dead spot inside it is finding 10.
- **`SevenSegDisplay` not inheriting `GpioOutput`** — correct. It owns two pins in a bit-banged
  protocol, not one digital I/O line; static-helper reuse of `GpioOutput::isValidOutputPin()` is
  composition, not an is-a relationship.
- **`Logger` / `AdapterFactory` as all-static classes** — considered for the Phase H2 item AA
  namespace conversion and rejected: item AA's saving was the `__cxa_guard_acquire/release`
  overhead of *Meyers* singletons. `Logger::currentLevel`/`AdapterFactory::isDevMode_` are plain
  eagerly-initialized static class members with no guard to remove, so a conversion would be pure
  style with no evidenced flash/RAM benefit.
- **`hw_mac.h`, `mac_table.h`, `MacEq.h`, `mem.h`, `error/{Error,ErrorCodes,ErrorCore}`,
  `app/{BootManager,DisplayManager}.h`** — read in full, single-purpose, several explicitly the
  product of prior de-duplication rounds (Phase G items F/N/Q, Phase H2 items Y/Z, Phase J).
  Recorded as confirmed-correct rather than skipped.

## File census

Scope: `firmware/main/src/**/*.{cpp,h,hpp}` (excluding `src/mesh/serialization/nanopb/` and the
generated `mesh.pb.h`/`mesh.pb.c`) plus the two top-level files `firmware/main/main.cpp` and
`firmware/main/project_config.h`. 57 files total (55 in `src/` + 2 top-level).

`Flag = investigate` is set for every file over ~150 lines, plus a small number of files under
that line count flagged on a judgment call (name/content suggests a second concern, or a
documented dead/overlapping-responsibility smell) — see Task 1 report for rationale on each.
This is not a hard-cutoff rule; Tasks 2-4 should still use their own judgment within flagged
files and may flag additional files if they find something during the deep look.

| File | Lines | Flag |
|---|---|---|
| `firmware/main/src/mesh/Mesh.cpp` | 1382 | investigate |
| `firmware/main/src/persistence/EepromManager.cpp` | 651 | investigate |
| `firmware/main/main.cpp` | 579 | investigate |
| `firmware/main/src/mesh/Mesh.h` | 495 | investigate |
| `firmware/main/src/adapter/serial/SerialAdapter.cpp` | 310 | investigate |
| `firmware/main/src/adapter/Adapter.cpp` | 245 | investigate |
| `firmware/main/src/mesh/Enrollment.cpp` | 224 | investigate |
| `firmware/main/src/adapter/serial/SerialFraming.cpp` | 209 | investigate |
| `firmware/main/src/mesh/NeighborTable.h` | 203 | investigate |
| `firmware/main/src/hardware/output/SevenSegDisplay.cpp` | 200 | investigate |
| `firmware/main/project_config.h` | 185 | investigate |
| `firmware/main/src/mesh/PeerRegistry.cpp` | 184 | investigate |
| `firmware/main/src/hardware/output/Led.cpp` | 162 | investigate |
| `firmware/main/src/crypto/Crypto.h` | 159 | investigate |
| `firmware/main/src/persistence/EepromManager.h` | 156 | investigate |
| `firmware/main/src/error/ErrorCore.cpp` | 142 | - |
| `firmware/main/src/adapter/Adapter.h` | 140 | - |
| `firmware/main/src/adapter/pir/PirAdapter.cpp` | 138 | - |
| `firmware/main/src/app/ButtonHandler.h` | 126 | investigate |
| `firmware/main/src/logging/Logger.h` | 117 | - |
| `firmware/main/src/mesh/E2ECrypto.h` | 110 | - |
| `firmware/main/src/mesh/E2EKeyStore.h` | 103 | - |
| `firmware/main/src/error/Error.h` | 103 | - |
| `firmware/main/src/adapter/AdapterFactory.cpp` | 103 | - |
| `firmware/main/src/mesh/CompactMessage.h` | 96 | - |
| `firmware/main/src/mesh/ReplayCache.h` | 92 | - |
| `firmware/main/src/mesh/Enrollment.h` | 89 | - |
| `firmware/main/src/mesh/RouteTable.h` | 88 | - |
| `firmware/main/src/adapter/serial/SerialAdapter.h` | 88 | - |
| `firmware/main/src/network/mac_table.h` | 81 | - |
| `firmware/main/src/logging/Logger.cpp` | 79 | - |
| `firmware/main/src/mesh/RouteMac.h` | 74 | - |
| `firmware/main/src/network/hw_mac.h` | 71 | - |
| `firmware/main/src/error/ErrorCore.h` | 68 | - |
| `firmware/main/src/adapter/pir/PirAdapter.h` | 68 | - |
| `firmware/main/src/hardware/input/Pir.cpp` | 66 | - |
| `firmware/main/src/app/DisplayManager.h` | 65 | - |
| `firmware/main/src/hardware/output/Led.h` | 60 | - |
| `firmware/main/src/mesh/PeerRegistry.h` | 57 | - |
| `firmware/main/src/adapter/AdapterFactory.h` | 50 | - |
| `firmware/main/src/network/MacAddress.h` | 49 | investigate |
| `firmware/main/src/mesh/CompactMessage.cpp` | 49 | - |
| `firmware/main/src/hardware/input/Pir.h` | 48 | - |
| `firmware/main/src/mesh/MeshCrypto.h` | 47 | - |
| `firmware/main/src/adapter/serial/SerialFraming.h` | 46 | - |
| `firmware/main/src/hardware/output/SevenSegDisplay.h` | 45 | - |
| `firmware/main/src/hardware/input/Button.cpp` | 42 | - |
| `firmware/main/src/hardware/input/Button.h` | 36 | - |
| `firmware/main/src/app/BootManager.h` | 35 | - |
| `firmware/main/src/hardware/input/GpioInput.h` | 32 | - |
| `firmware/main/src/hardware/input/GpioInput.cpp` | 32 | - |
| `firmware/main/src/hardware/output/GpioOutput.cpp` | 31 | - |
| `firmware/main/src/hardware/output/GpioOutput.h` | 28 | - |
| `firmware/main/src/error/ErrorCodes.h` | 27 | - |
| `firmware/main/src/network/MacEq.h` | 25 | - |
| `firmware/main/src/network/mem.h` | 21 | - |
| `firmware/main/src/mesh/broadcast_mac.h` | 16 | - |

### Judgment calls on sub-150-line flags

- **`firmware/main/src/app/ButtonHandler.h` (126 lines, flagged):** highest fan-in of any
  file under the size threshold — pulls in `Mesh`, `EepromManager`, `Led`, `Button`, and
  `Logger` to coordinate a hold-to-configure/reset gesture across hardware, mesh, and
  persistence layers in one static `tick()`. Cohesive in intent (it's the app's one button
  orchestrator) but broad enough in responsibility, and close enough to the size threshold,
  that Task 3 (non-mesh audit) should give it a real look rather than a skim.
  *Outcome: finding 13.*
- **`firmware/main/src/network/MacAddress.h` (49 lines, flagged):** `MacEq.h`'s own header
  comment documents that the `lattice::utils::MacAddress` equality path is "strictly worse"
  than the `mac::eq()` free function it now delegates to (extra temporary-object copies), and
  `MacAddress::toString()` carries an explicit "no callers exist yet" comment. That's a
  self-documented partial-dead-code / superseded-idiom smell worth a deliberate decision
  (keep as the mesh's one MAC value type, or trim) rather than a silent skip.
  *Outcome: finding 7 — delete.*

All other sub-150-line files were skimmed (first ~15-25 lines and any suspicious neighbors,
e.g. the `network/` MAC-helper cluster and the `mesh/*Crypto*.h` cluster) and found to be
single-purpose, several explicitly documented as prior de-duplication work (post-Phase-G,
Phase H2). They're left unflagged; Tasks 2-4 remained free to flag any of them if a closer read
turned up something that pass missed — none did.

## Session provenance

Phase A ran as a 5-task plan (`docs/superpowers/plans/2026-08-07-phaseA-clean-code-audit.md`) on
2026-08-07: Task 1 the file census, Tasks 2-4 three independent subsystem/library audits
dispatched in parallel worktrees writing conflict-free fragment files, Task 5 this synthesis. Each
of Tasks 1-4 passed an independent code review before merge (all Approved; minor issues logged in
`.superpowers/sdd/2026-08-07-phaseA-clean-code-audit/progress.md`). Every `File:line` citation in
this ledger was grep-verified against the tree by its originating task and re-verified in that
task's review; the fragment files
(`docs/superpowers/specs/.audit-fragments/{mesh,nonmesh,library}-findings.md`) were folded into
this document and deleted.

Minor issues logged during review and fixed in this merge: finding 5 gained the missing
`Mesh.cpp:1140` citation site; finding 3's headline count is now a single consistent "15+"
(the fragment said "10+", the task report "15+" — the report's full enumeration is the accurate
one); findings 15 and 23's paraphrased quotes were de-quoted; the Architecture-boundary section's
undercounts were replaced with re-verified numbers (41 `EepromManager` functions, 18 distinct
`eeprom::` functions across 22 call sites from `mesh/` alone, ~41 files in hub's
`orchestrator/mesh/`); and the `MacAddress` cross-task overlap between the mesh and non-mesh
audits was reconciled into the single finding 7 rather than duplicated.
