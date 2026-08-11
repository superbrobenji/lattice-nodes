# Phase E — Array-in-Interface Consistency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve all 7 live `cpp/array-in-interface` CodeQL alerts (6 dismissed, 1 open) by
rewriting ~17 function signatures from array-bracket (`T x[N]`) to plain pointer (`T* x`) syntax —
identical at the type/ABI level, zero call-site changes, zero behavior change.

**Architecture:** Pure signature-syntax normalization across 9 files. No new types, no new
collaborators, no call-site edits anywhere (array-to-pointer decay makes every existing call site
already compatible with the new signature).

**Tech Stack:** ESP-IDF C++17.

## Global Constraints

- Firmware-only, no wire-format changes, no backwards-compat shims.
- Zero behavior change — this is a pure syntax change. Every parameter keeps its exact name,
  order, and const-qualification; only the array-bracket suffix (`[N]`) is removed and the type
  becomes a pointer.
- No call sites change. If a step in this plan appears to require touching a caller, stop and
  re-check — the design spec's whole premise is that decay makes this unnecessary; a caller edit
  means something was miscounted as decay-equivalent when it wasn't.
- Full unit + e2e regression required, same-or-better test count as before (no new tests needed —
  existing tests already exercise every touched function's behavior, which doesn't change).
- clang-format v18 pinned: `/opt/homebrew/opt/llvm@18/bin/clang-format`.

---

### Task 1: Rewrite all array-bracket parameters to pointer syntax

**Files:**
- Modify: `firmware/main/src/mesh/DownlinkRouter.h`, `firmware/main/src/mesh/DownlinkRouter.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h`, `firmware/main/src/mesh/Mesh.cpp`
- Modify: `firmware/main/src/mesh/MeshTransport.h`, `firmware/main/src/mesh/MeshTransport.cpp`
- Modify: `firmware/main/src/mesh/MasterBeacon.h`, `firmware/main/src/mesh/MasterBeacon.cpp`
- Modify: `firmware/main/src/mesh/RouteMac.h`
- Modify: `firmware/main/src/crypto/Crypto.h`
- Modify: `firmware/main/src/network/mac_table.h`
- Modify: `firmware/main/src/network/hw_mac.h`
- Modify: `firmware/main/src/adapter/Adapter.h`, `firmware/main/src/adapter/Adapter.cpp`

**Interfaces:** No new interfaces. Every existing function keeps its exact name and call-site
usage — only the declared parameter type changes from `T[N]` to `T*` (or `const T[N]` to
`const T*`).

- [ ] **Step 1: `mesh/DownlinkRouter.h` + `.cpp` — `classify`**

In `DownlinkRouter.h`, change:
```cpp
  RouteDecision classify(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                         bool addressedToSelf, bool isBroadcastTarget, bool addressedToMaster,
                         uint8_t nextHopMacOut[6]) const;
```
to:
```cpp
  RouteDecision classify(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                         bool addressedToSelf, bool isBroadcastTarget, bool addressedToMaster,
                         uint8_t* nextHopMacOut) const;
```

In `DownlinkRouter.cpp`, change:
```cpp
RouteDecision DownlinkRouter::classify(const mesh_message& msg, const uint8_t* deviceMac,
                                       bool isMaster, bool addressedToSelf, bool isBroadcastTarget,
                                       bool addressedToMaster, uint8_t nextHopMacOut[6]) const {
```
to:
```cpp
RouteDecision DownlinkRouter::classify(const mesh_message& msg, const uint8_t* deviceMac,
                                       bool isMaster, bool addressedToSelf, bool isBroadcastTarget,
                                       bool addressedToMaster, uint8_t* nextHopMacOut) const {
```

- [ ] **Step 2: `mesh/Mesh.h` + `.cpp` — `handleReceivedMessage`, `handleReceivedMessageTrampoline`**

In `Mesh.h`, change:
```cpp
  void handleReceivedMessage(const uint8_t srcMac[6], const mesh_message& msg);
```
to:
```cpp
  void handleReceivedMessage(const uint8_t* srcMac, const mesh_message& msg);
```

And change:
```cpp
  static void handleReceivedMessageTrampoline(const uint8_t srcMac[6], const mesh_message& msg) {
```
to:
```cpp
  static void handleReceivedMessageTrampoline(const uint8_t* srcMac, const mesh_message& msg) {
```

In `Mesh.cpp`, change:
```cpp
void Mesh::handleReceivedMessage(const uint8_t srcMac[6], const mesh_message& msg) {
```
to:
```cpp
void Mesh::handleReceivedMessage(const uint8_t* srcMac, const mesh_message& msg) {
```

- [ ] **Step 3: `mesh/MeshTransport.h` + `.cpp` — `MessageHandler` typedef, `registerPeerWithEspNow`**

Must change together with Step 2 — `MessageHandler` is the function-pointer type
`handleReceivedMessageTrampoline` is bound to; both sides of that binding must have matching
signatures at all times (they will, immediately after this step, since both changed in this same
task).

In `MeshTransport.h`, change:
```cpp
  using MessageHandler = void (*)(const uint8_t srcMac[6], const mesh_message& msg);
```
to:
```cpp
  using MessageHandler = void (*)(const uint8_t* srcMac, const mesh_message& msg);
```

And change:
```cpp
  static void registerPeerWithEspNow(const uint8_t mac[6]);
```
to:
```cpp
  static void registerPeerWithEspNow(const uint8_t* mac);
```

In `MeshTransport.cpp`, change:
```cpp
void MeshTransport::registerPeerWithEspNow(const uint8_t mac[6]) {
```
to:
```cpp
void MeshTransport::registerPeerWithEspNow(const uint8_t* mac) {
```

- [ ] **Step 4: `mesh/MasterBeacon.h` + `.cpp` — `checkTimeout`, `process`**

In `MasterBeacon.h`, change:
```cpp
  void checkTimeout(bool isMaster, MasterInfo& currentMaster, uint8_t lastSeenMasterMac[6]);
```
to:
```cpp
  void checkTimeout(bool isMaster, MasterInfo& currentMaster, uint8_t* lastSeenMasterMac);
```

And change:
```cpp
  void process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
               bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
               MasterInfo& currentMaster, OutboundSequenceState& txState,
               mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut, bool& relayPendingOut,
               uint8_t lastSeenMasterMac[6]);
```
to:
```cpp
  void process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
               bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
               MasterInfo& currentMaster, OutboundSequenceState& txState,
               mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut, bool& relayPendingOut,
               uint8_t* lastSeenMasterMac);
```

In `MasterBeacon.cpp`, change:
```cpp
void MasterBeacon::checkTimeout(bool isMaster, MasterInfo& currentMaster,
                                uint8_t lastSeenMasterMac[6]) {
```
to:
```cpp
void MasterBeacon::checkTimeout(bool isMaster, MasterInfo& currentMaster,
                                uint8_t* lastSeenMasterMac) {
```

And change:
```cpp
void MasterBeacon::process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                           bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
                           MasterInfo& currentMaster, OutboundSequenceState& txState,
                           mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut,
                           bool& relayPendingOut, uint8_t lastSeenMasterMac[6]) {
```
to:
```cpp
void MasterBeacon::process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                           bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors,
                           MasterInfo& currentMaster, OutboundSequenceState& txState,
                           mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut,
                           bool& relayPendingOut, uint8_t* lastSeenMasterMac) {
```

- [ ] **Step 5: `mesh/RouteMac.h` — `buildHopContext`, `chainStep`**

Change:
```cpp
inline void buildHopContext(const mesh_message& msg, const uint8_t prev_hop[6],
                            const uint8_t this_hop[6], uint8_t out_ctx[HOP_CTX_LEN]) {
```
to:
```cpp
inline void buildHopContext(const mesh_message& msg, const uint8_t* prev_hop,
                            const uint8_t* this_hop, uint8_t* out_ctx) {
```

Change:
```cpp
inline void chainStep(const uint8_t secret[32], const uint8_t hop_ctx[HOP_CTX_LEN],
                      const uint8_t prev_mac[AUTH_PATH_LEN], uint8_t out_mac[AUTH_PATH_LEN]) {
```
to:
```cpp
inline void chainStep(const uint8_t* secret, const uint8_t* hop_ctx,
                      const uint8_t* prev_mac, uint8_t* out_mac) {
```

- [ ] **Step 6: `crypto/Crypto.h` — `reverse32`, `x25519_keygen`, `x25519_shared`, `hmac_sha256`, `aead_seal`, `aead_open`**

Change:
```cpp
inline void reverse32(const uint8_t in[32], uint8_t out[32]) {
```
to:
```cpp
inline void reverse32(const uint8_t* in, uint8_t* out) {
```

Change:
```cpp
inline bool x25519_keygen(uint8_t priv32BE[32], uint8_t pub32BE[32]) {
```
to:
```cpp
inline bool x25519_keygen(uint8_t* priv32BE, uint8_t* pub32BE) {
```

Change:
```cpp
inline bool x25519_shared(const uint8_t priv32BE[32], const uint8_t peerPub32BE[32],
                          uint8_t secret32[32]) {
```
to:
```cpp
inline bool x25519_shared(const uint8_t* priv32BE, const uint8_t* peerPub32BE,
                          uint8_t* secret32) {
```

Change:
```cpp
inline bool hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                        uint8_t out32[32]) {
```
to:
```cpp
inline bool hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                        uint8_t* out32) {
```

Change:
```cpp
inline bool aead_seal(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, uint8_t tag16[16]) {
```
to:
```cpp
inline bool aead_seal(const uint8_t* key32, const uint8_t* nonce12, const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, uint8_t* tag16) {
```

Change:
```cpp
inline bool aead_open(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, const uint8_t tag16[16]) {
```
to:
```cpp
inline bool aead_open(const uint8_t* key32, const uint8_t* nonce12, const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, const uint8_t* tag16) {
```

Note: `reverse32` lives inside `namespace detail` (an internal helper `x25519_shared` calls as
`detail::reverse32`) — still in scope, CodeQL's rule doesn't care about namespace-level visibility
conventions, only the AST's array-type declarator.

- [ ] **Step 7: `network/mac_table.h` — `find`**

Change:
```cpp
inline size_t find(const void* entries, size_t n, size_t stride, size_t mac_offset,
                   const uint8_t mac[6]) {
```
to:
```cpp
inline size_t find(const void* entries, size_t n, size_t stride, size_t mac_offset,
                   const uint8_t* mac) {
```

- [ ] **Step 8: `network/hw_mac.h` — `cacheDeviceMac`, `readOwnMac` (both `#ifdef UNIT_TEST` branches)**

In the non-`UNIT_TEST` branch, change:
```cpp
inline void cacheDeviceMac(const uint8_t mac[6]) {
  memcpy(detail::g_deviceMac, mac, 6);
  detail::g_deviceMacCached = true;
}

inline void readOwnMac(uint8_t out[6]) {
```
to:
```cpp
inline void cacheDeviceMac(const uint8_t* mac) {
  memcpy(detail::g_deviceMac, mac, 6);
  detail::g_deviceMacCached = true;
}

inline void readOwnMac(uint8_t* out) {
```

In the `UNIT_TEST` branch, change:
```cpp
inline void cacheDeviceMac(const uint8_t[6]) {}

inline void readOwnMac(uint8_t out[6]) {
```
to:
```cpp
inline void cacheDeviceMac(const uint8_t*) {}

inline void readOwnMac(uint8_t* out) {
```

- [ ] **Step 9: `adapter/Adapter.h` + `.cpp` — `sendDataThroughMesh`**

In `Adapter.h`, change:
```cpp
  void sendDataThroughMesh(const adapter_types type,
                           const uint8_t data[64]); // sends data through mesh
```
to:
```cpp
  void sendDataThroughMesh(const adapter_types type,
                           const uint8_t* data); // sends data through mesh
```

In `Adapter.cpp`, change:
```cpp
void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[64]) {
```
to:
```cpp
void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t* data) {
```

- [ ] **Step 10: Build and test**

Run: `cmake --build tests/build --parallel 2 && ctest --test-dir tests/build --output-on-failure --label-exclude e2e && ctest --test-dir tests/build --output-on-failure --label-regex e2e`
Expected: identical counts to before this task (299 unit + 41 e2e) — this is a pure signature
change with zero behavior delta. Any failure means a call site was not actually decay-compatible
and needs investigation before proceeding, not a blind retry.

- [ ] **Step 11: Format check**

Run `/opt/homebrew/opt/llvm@18/bin/clang-format --dry-run --Werror` against every file touched in
Steps 1-9. Reformat in place if needed (`-i` instead of `--dry-run --Werror`) and re-verify the
build/tests still pass.

- [ ] **Step 12: Commit**

```bash
git add firmware/main/src/mesh/DownlinkRouter.h firmware/main/src/mesh/DownlinkRouter.cpp \
        firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp \
        firmware/main/src/mesh/MeshTransport.h firmware/main/src/mesh/MeshTransport.cpp \
        firmware/main/src/mesh/MasterBeacon.h firmware/main/src/mesh/MasterBeacon.cpp \
        firmware/main/src/mesh/RouteMac.h \
        firmware/main/src/crypto/Crypto.h \
        firmware/main/src/network/mac_table.h \
        firmware/main/src/network/hw_mac.h \
        firmware/main/src/adapter/Adapter.h firmware/main/src/adapter/Adapter.cpp
git commit -m "refactor(phaseE): array-bracket parameters -> pointer syntax (cpp/array-in-interface)"
```

## Self-Review Notes

- **Spec coverage:** all 17 functions across all 9 files from the design spec's table are covered
  (Steps 1-9 map 1:1 to the spec's file list).
- **Placeholder scan:** no TBD/TODO; every step shows exact before/after code.
- **Type consistency:** every signature change is purely `T[N]`/`T[N]` → `T*`/`T*` — no parameter
  renamed, reordered, or reordered across steps.
- **Verification:** the plan does not touch a single call site anywhere — this is intentional and
  is the design's core claim (decay makes callers already-compatible). Step 10's regression run is
  what actually proves that claim true, not just asserts it.
