# Task 8 Report — Ring buffers → xRingbufferCreateStatic (item OO)

**Status:** Complete.

**Branch:** `feat/phaseI-task8-xringbuffer` (off `origin/docs/phaseI-native-idf` @ `8b39be3`), pushed to origin. No PR opened (per instructions — orchestrator handles).

**Commit:** `eec4012` — "feat(phaseI/task8): ring buffers -> xRingbufferCreateStatic (item OO)"

## Test results

- **Host tests: 296/296 pass** (`cd tests && cmake --build build -j2 && ctest --test-dir build --parallel 1`), pin file removed first (`rm -f firmware/main/config/master_pubkey_pin.h`).
- **ESP-IDF build: clean.** `idf.py reconfigure && idf.py build` (env: `IDF_PATH=$HOME/esp/esp-idf`, IDF v5.5.1, `-j2`-equivalent — build system default parallelism was used for `idf.py build` itself since it isn't a raw `cmake --build` invocation, but the host test build honored the `-j2` cap per constraints) succeeds with no errors. `idf.py size` runs clean.
- Firmware submodule `firmware/main/lib/lattice-protocol` was not initialized in this fresh worktree; ran `git submodule update --init --recursive` before the first build (same issue flagged in the Task 5 report for parallel worktrees).

## Size delta (`idf.py size`, isolated via `git stash` of the tracked source changes to get a true before/after on this branch)

| Section | Baseline (8b39be3) | Post-Task-8 | Delta |
|---|---|---|---|
| .text (Flash Code) | 579,260 | 579,108 | -152 |
| .rodata (Flash Data) | 118,288 | 118,288 | 0 |
| IRAM (.text + .vectors) | 92,831 | 92,803 | -28 |
| .bss | 21,016 | 21,496 | +480 |
| .data | 16,196 | 16,196 | 0 |
| **Total image** | 806,863 | 806,683 | **-180** |

Net roughly neutral/slight win, matching the plan's expectation. `.text`/IRAM drop slightly (~180 bytes combined) from removing the hand-rolled modulo-wraparound arithmetic in `onDataRecvCallback`/`drainRecvQueue`/`enqueuePendingRelay`/`drainPendingRelay`. `.bss` grows +480 bytes: each `xRingbufferCreateStatic` ring carries a `StaticRingbuffer_t` control-block (read/write pointers, free-space bookkeeping — ESP-IDF's ring buffer internals, not something firmware code lays out) on top of the storage array, and two rings now exist (`Mesh::recvQueue` + `Enrollment::_pendingRelayQueue`) versus the old design's 2-byte head/tail scalars. The two per-item-header-overhead pads (`+128` each, per the plan's snippet) are generous relative to actual overhead (real NOSPLIT per-item header is ~8 bytes × 4 slots = 32 bytes needed) but were kept as specified rather than hand-tuned down, since the net delta is already negative/neutral.

## What changed

- `firmware/main/src/mesh/Mesh.h` — `recvQueue[RECV_QUEUE_SIZE]` + `recvQueueHead`/`recvQueueTail` replaced with `RingbufHandle_t recvQueue` + `StaticRingbuffer_t _recvQueueStruct` + `uint8_t _recvQueueStorage[RECV_QUEUE_SIZE * sizeof(RecvQueueEntry) + 128]`. Added `#include <freertos/FreeRTOS.h>` + `#include <freertos/ringbuf.h>`. `SIMULATE_MODE`'s `injectReceivedMessage()` now does a non-blocking `xRingbufferSend` instead of manual head-increment (same silent-drop-on-full semantics).
- `firmware/main/src/mesh/Mesh.cpp`:
  - Constructor: creates the ring via `xRingbufferCreateStatic` (removed `recvQueueHead(0)`/`recvQueueTail(0)` from the init list and the `memset(recvQueue, ...)` call — the ring's storage doesn't need pre-zeroing).
  - `onDataRecvCallback` (WiFi-task/ISR context): builds a local `RecvQueueEntry`, sends it via `xRingbufferSendFromISR(..., &woken)`, calls `portYIELD_FROM_ISR()` if woken. Queue-full still silently drops (no log call — this runs in ISR context, and the codebase's own comments elsewhere note Serial writes are unsafe there).
  - `drainRecvQueue`: loops `xRingbufferReceive(recvQueue, &itemSize, 0)` until null, copies the item to a local `RecvQueueEntry` (`entry = *entryPtr`), calls `vRingbufferReturnItem` **before** dispatching (the ring's item memory is only guaranteed valid until returned — see bug note below), then dispatches exactly as before (proto-version check, replay check, `switch` on message type).
- `firmware/main/src/mesh/Enrollment.h` — `_pendingRelayQueue[PENDING_RELAY_QUEUE_SIZE]` + `_pendingRelayHead`/`_pendingRelayCount` replaced with `RingbufHandle_t _pendingRelayQueue` + `StaticRingbuffer_t _pendingRelayQueueStruct` + `uint8_t _pendingRelayQueueStorage[PENDING_RELAY_QUEUE_SIZE * sizeof(PendingRelay) + 128]`. Same two includes added.
- `firmware/main/src/mesh/Enrollment.cpp`:
  - Constructor: creates the ring via `xRingbufferCreateStatic`.
  - `enqueuePendingRelay`: builds a local `PendingRelay`, sends via `xRingbufferSend(..., 0)` (non-blocking — task context, called from `Mesh::drainRecvQueue()`, not ISR), logs the existing "queue full — dropping" `LOG_WARN` if the send fails.
  - `drainPendingRelay`: loops `xRingbufferReceive` until null, invokes `_enrollmentRelayFn` (if set) with the item's `mac`/`pubKey` pointers **before** `vRingbufferReturnItem` (production callback — `SerialAdapter::relayEnrollmentToServer` — already copies synchronously via `memcpy`, so this is safe), then returns the item.
- `tests/mocks/freertos/FreeRTOS.h` (new) — shadow header for `<freertos/FreeRTOS.h>`: `BaseType_t`/`UBaseType_t`/`TickType_t`, `pdTRUE`/`pdFALSE`/`portMAX_DELAY`, and a no-op `portYIELD_FROM_ISR(...)` (nothing to yield to on host).
- `tests/mocks/freertos/ringbuf.h` (new) — shadow header for `<freertos/ringbuf.h>`: `RingbufferType_t`, `StaticRingbuffer_t` (opaque placeholder), `RingbufHandle_t` (pointer to an internal `RingbufMockState`), and `xRingbufferCreateStatic`/`xRingbufferSend`/`xRingbufferSendFromISR`/`xRingbufferReceive`/`vRingbufferReturnItem`, all `inline` — no companion `.cpp` needed, so no `tests/CMakeLists.txt` change was required.
- `tests/unit/test_route_report.cpp` — the one direct-array-poke test site (`DrainRecvQueue_DispatchesRouteReport`) now builds a `Mesh::RecvQueueEntry` and pushes it via `xRingbufferSend(mesh.recvQueue, &entry, sizeof(entry), 0)` instead of manual head-index writes.
- `tests/unit/test_mesh_logic.cpp`:
  - `DrainRecvQueueTest::injectAndDrain` helper — same `xRingbufferSend` treatment as above.
  - All `mesh.enrollment._pendingRelayCount`/`_pendingRelayHead` assertions rewritten to read `mesh.enrollment._pendingRelayQueue->items.size()` / `->items.front()` directly off the mock's internal state (the mock's `RingbufMockState` fields are plain public members, reachable in `UNIT_TEST` builds the same way `recvQueue` itself is exposed — see "Mock strategy" below).
  - `EnrollmentRelayCallbackTest`'s `captureRelayFn` test helper — see bug note.

## Mock strategy

`RingbufHandle_t` is a pointer to an internal `RingbufMockState` struct (capacity/used-bytes counters + a `std::deque<std::vector<uint8_t>>` of item copies) — not `void*`, and the struct's fields are plain public members (default `struct` visibility), so host tests reach into `mesh.enrollment._pendingRelayQueue->items.size()` / `.front()` the same way the pre-existing `UNIT_TEST`-exposes-everything convention already let tests reach into `mesh.recvQueue`/`mesh.recvQueueHead` directly. This kept the test-adaptation surface small — no new accessor methods needed on `Mesh`/`Enrollment`.

Capacity accounting in the mock (`usedBytes + itemSize + 8 > capacityBytes` → reject) mirrors the real NOSPLIT ring's per-item header overhead closely enough that the existing "queue full — drop" test paths (implicit in `RECV_QUEUE_SIZE`/`PENDING_RELAY_QUEUE_SIZE` = 4 throughout the suite) still exercise the same way under test as on-device.

`xRingbufferReceive` heap-allocates a private copy per call (`new uint8_t[]`) rather than pointing into a shared ring buffer the way the real ESP-IDF implementation does — this is transparent to callers (which never retain the pointer past their processing + `vRingbufferReturnItem` call in the correct/intended usage pattern) but is exactly what surfaced the bug below when a test violated that pattern.

## Bug found and fixed: use-after-free in a test helper (not production code)

While porting `tests/unit/test_mesh_logic.cpp`'s `EnrollmentRelayCallbackTest.DrainCallsRegisteredCallback`, the existing `captureRelayFn` test helper did:
```cpp
static void captureRelayFn(const uint8_t mac[6], const uint8_t pubKey[32]) {
  g_capturedMac = mac;   // stores the RAW POINTER, not a copy
  g_capturedKey = pubKey;
}
```
and the test asserted on `g_capturedMac`/`g_capturedKey` **after** `drainPendingRelay()` returned. Under the old array-backed queue this was safe by accident — a drained slot's backing memory (a fixed member array) stayed resident indefinitely, so a stale pointer into it still read valid (if logically "already consumed") bytes. Under the ring buffer, `vRingbufferReturnItem()` (called immediately after the callback returns, inside `drainPendingRelay`'s loop) frees/recycles that memory — so the pointers were dangling by the time the test read them. The test failed non-deterministically-looking but reproducibly: the very next `EXPECT_EQ(...)`'s gtest message-stream construction reused the just-freed heap block, corrupting the captured bytes.

This is not a production bug: the one real caller of this callback type, `SerialAdapter::relayEnrollmentToServer` (`firmware/main/src/adapter/serial/SerialAdapter.cpp:119`), already copies both buffers synchronously via `memcpy` inside the callback and never retains the pointers — exactly the contract the ring buffer requires. Fixed the test helper to copy into static byte arrays (`g_capturedMac[6]`/`g_capturedKey[32]`) during the callback instead of storing raw pointers, and reran the full suite to confirm no other test relied on stale-pointer semantics (there were none — this was the only such site, since the `Mesh::drainRecvQueue` production path itself already copies `*entryPtr` to a local before returning the item, and no test inspects `Mesh::recvQueue` contents post-drain by pointer).

## Concerns / notes

- **No `firmware/main/CMakeLists.txt` change needed** — `freertos` headers were already transitively reachable (via `arduino-esp32`/`esp_wifi`/`nvs_flash`, all already in `REQUIRES`); `idf.py build` succeeded without adding `freertos` explicitly.
- Ran a full `clang-format -i --style=file` pass on all four touched firmware source files (`Mesh.h`/`.cpp`, `Enrollment.h`/`.cpp`) and the two new mock headers — these diffs are clean (no pre-existing formatting drift). **Deliberately did NOT** run a whole-file `clang-format` pass on `tests/unit/test_mesh_logic.cpp` / `test_route_report.cpp`: a first attempt showed both files carry pre-existing formatting drift (double-space-before-trailing-comment, some line-wrap differences) from before they were last touched by clang-format 18, and a full-file reformat would have buried my actual logic changes in ~150+ lines of unrelated cosmetic diff. CI's `unit-tests.yml` clang-format gate only checks `firmware/main/src/**/*.{cpp,h}` (confirmed by reading the workflow), not `tests/`, so this is a compliant choice, not a shortcut — reverted the full-file reformat via `git checkout` and reapplied only the targeted edits.
- Size measurement methodology: captured post-Task-8 size first, then `git stash` of only the four mesh source files (kept test files staged/modified, irrelevant to the firmware build) to get a true incremental-rebuild baseline on the same toolchain/build-cache, then `git stash pop` to restore. Verified the final restored state still builds clean and passes 296/296 before committing.
- `firmware/dependencies.lock`, `firmware/managed_components/`, `firmware/sdkconfig` were generated by the ESP-IDF build in this worktree and are **not** committed (not covered by the repo's `.gitignore`, which only ignores `build/`, but were excluded via explicit `git add <paths>` rather than `git add -A`, matching the repo's stated staging discipline). Flagging in case the orchestrator wants these gitignore'd properly in a follow-up — every task that runs a real `idf.py build` will hit this.
