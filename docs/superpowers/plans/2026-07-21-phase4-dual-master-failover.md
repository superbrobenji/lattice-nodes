<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phase 4: Dual-Master Data Failover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After a node fails over from a silent primary master to a secondary master, its sealed uplink reaches — and is decryptable by — the secondary. Closes design gap #8.

**Architecture:** The primary's JOIN_ACK already-present-on-the-wire `secondary_master_mac`/`secondary_public_key` fields are populated by the server and passed through by the primary master. The enrolling leaf reads them and **registers the secondary master as a persisted PeerRegistry peer** (mac + pubkey) at enrollment — which is all `masterE2EKeys()` needs to derive a distinct `k_up`/`k_down` pair against the secondary after `currentMaster.mac` flips to it (the failover adoption itself already works and is tested). The secondary master obtains each leaf's pubkey via the same server→master enroll path it already has (server sync). No EEPROM-layout change (the secondary pubkey persists in the existing peer list), no wire-format change (fields already exist).

**Tech Stack:** C++ (ESP32 firmware, host-tested), GoogleTest + `tests/e2e` sim harness. `main/lib/lattice-protocol` submodule at v0.4.0 (unchanged — `secondary_master_mac[6]`/`secondary_public_key[32]` already in the v3 struct).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-16-multihop-routing-e2e-crypto-design.md` §5 (dual-master data failover), §2 (keying).
- **Confirmed assumption (from brainstorm):** both masters are hub/server-connected. The server designates the secondary and supplies its MAC+pubkey in the JOIN_ACK it relays through the primary; the server also syncs each enrolled leaf's pubkey to the secondary master.
- **No wire-format / protocol change:** `secondary_master_mac`/`secondary_public_key` already exist in `mesh_message` (v3, `sizeof==242`). This phase only starts reading/writing them.
- **No EEPROM-layout change (intentional deviation from spec §5's "a pubkey slot is added"):** spec §5 sketched persisting the secondary pubkey in a new EEPROM slot, but only 9 bytes are free (a 32-byte slot doesn't fit without restructuring). Instead the secondary master is stored as a normal `PeerRegistry` peer (persisted mac+pubkey via the existing `PEER_LIST` region, `MAX_PEERS=10`). This is not merely storage-convenient: registering the secondary as a peer is precisely what lets `masterE2EKeys()`'s `peers.find(currentMaster.mac)` resolve the secondary's key post-failover with **zero change** to the key-lookup path. The secondary MAC also persists in the existing `KNOWN_MASTER_MAC_SECONDARY` slot (EepromManager, addr 497) via the existing `saveKnownMasterMacSecondary`. Do NOT add a new EEPROM slot.
- **PeerRegistry enrollment-only rule is preserved:** registering the secondary is an *enrollment-path* add (driven by the server-authenticated JOIN_ACK), through the same `registerPeerWithKey(allowRekey=false)` used for the primary. Do NOT weaken the JOIN_ACK re-key gating (Task-7 security from the e2e suite: an unauthenticated JOIN_ACK must not replace an established key; first-contact/all-zero placeholder is still upgradable).
- **Key derivation is master-agnostic:** `masterE2EKeys()` keys off `currentMaster.mac` + `peers.find(currentMaster.mac)->publicKey`; `E2EKeyStore` caches per-MAC, so a secondary present in `peers` yields a distinct `k_up`/`k_down` automatically. The nonce (`epoch||seq||origin_mac`) is the leaf's own and master-agnostic — switching masters cannot cause nonce reuse (different key). Uplink uses `k_up` regardless of which master. Do NOT change `masterE2EKeys`/`peerE2EKeys`/nonce logic.
- Config: `DUAL_MASTER_MODE` (`project_config.h`, default false) gates dual-master behavior; unchanged.
- All firmware test hooks `#ifdef UNIT_TEST`-gated.
- **clang-format 18** (CI `lint-format` checks `main/src` only). `python3 -m pip install 'clang-format==18.1.8'`; `CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")`; check changed `main/src` files with `"$CF" --style=file --dry-run --Werror <files>`. Only reformat changed lines. Local `ctest` does NOT run clang-format.
- **CodeQL:** params `const uint8_t*` not `const uint8_t[6]`; bounds-check buffer indexing.
- Verification loop: `cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release` (once), `cmake --build tests/build --parallel`, `ctest --test-dir tests/build --output-on-failure`. Baseline **176 pass / 0 disabled**.

## Current-state facts (verified on this branch, post-Phase-3)

- `secondary_master_mac`/`secondary_public_key` in `mesh_message` (`mesh_message.h:25-26`) are dead (never read/written anywhere).
- `Enrollment::processJoinAck(const mesh_message& msg, const uint8_t* deviceMac, RegisterPeerFn registerFn)` (`Enrollment.h:32`, body `Enrollment.cpp:92-134`): verifies the `data[0..3]` fingerprint, TOFU-gates on `knownMasterMac`, calls `registerFn(msg.origin_mac_address, msg.enrollment_public_key)` to register the **primary**, sets enrolled flag + TOFU-learns `knownMasterMac`. Never reads the secondary fields. `RegisterPeerFn = std::function<bool(const uint8_t* mac, const uint8_t* pubKey32)>` (`Enrollment.h:12`).
- `Enrollment` dual-master state (`Enrollment.h:17-20`): `hasMasterMac`/`knownMasterMac[6]`, `hasMasterMacSecondary`/`knownMasterMacSecondary[6]`. `init()` loads both from EEPROM.
- `Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32)` (`Mesh.h:320`, body `Mesh.cpp:1015-1047`): master-side; registers the peer via `registerPeerWithKey(...allowRekey=true)`, then builds+broadcasts a JOIN_ACK — sets `ack.enrollment_public_key = enrollment.getPublicKey()` (own pubkey), `ack.data[0..3]` fingerprint; leaves `ack.secondary_*` zero (`mesh_message ack = {}`).
- `masterE2EKeys` (`Mesh.cpp:413-420`): `peers.find(currentMaster.mac)` → derives via `e2eKeys.getKeys(mac, ownPriv, master->publicKey, ...)`. Returns false if the master isn't in `peers` — the exact gap-#8 failure after failover (secondary never in `peers`). No change needed once the secondary IS in `peers`.
- `processMasterBeacon` dual-master adoption (`Mesh.cpp:659-765`): TOFU-learns `knownMasterMacSecondary` (if `_dualMasterMode && !hasMasterMacSecondary`) and adopts `currentMaster.mac = secondary` on failover. Works and is tested.
- Master serial JOIN_ACK handler (`SerialAdapter.cpp:241-260`): reads `msg.enrollment_public_key`, calls `meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key)`. The server relays approval as a full 242-byte `mesh_message` (so `secondary_*` fields are already carried over serial; the handler just ignores them).
- `FakeHub::approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32)` (`FakeHub.h:43`): builds a JOIN_ACK with only target + `enrollment_public_key` + fingerprint. `FakeHub(SimNode* master)` models one hub. `MeshSimTest::enroll(node)` (`MeshSimTest.h:57`) calls `hub->approveEnrollment(node->mac(), req->enrollment_public_key)`.
- `test_dual_master_e2e.cpp`: two ENABLED tests (`FailsOverToSecondaryMasterWhenPrimaryGoesSilent` asserts route adoption only; `LearnsSecondaryMasterInDualMode`). **No data-failover (decryption) test exists** — gap #8's executable spec must be authored. `bringUpSecondary` adds `master2` (isMaster, SERIAL_ADAPTER) linked to sensor + primary, but attaches NO hub to it.
- `E2EKeyStore::getKeys(const uint8_t* mac, const uint8_t* ownPriv32, const uint8_t* peerPub32, const uint8_t** kUpOut, const uint8_t** kDownOut)` caches per-MAC — a secondary MAC yields its own cached pair automatically.

## File Structure

- **Modify** `main/src/mesh/Mesh.h`/`Mesh.cpp` — `enrollPeer` gains an overload carrying the secondary identity; JOIN_ACK stamps `secondary_*`.
- **Modify** `main/src/mesh/Enrollment.h`/`Enrollment.cpp` — `processJoinAck` reads `secondary_*`, registers the secondary peer, sets/persists secondary TOFU state.
- **Modify** `main/src/adapter/serial/SerialAdapter.cpp` — pass the server-provided `secondary_*` fields through to `enrollPeer`.
- **Modify** `tests/e2e/harness/FakeHub.h`/`FakeHub.cpp` — `approveEnrollment` overload that populates `secondary_*`.
- **Modify** `tests/e2e/scenarios/test_dual_master_e2e.cpp` — new data-failover e2e test (gap #8 spec).
- **Modify** `tests/unit/test_mesh_logic.cpp` (and/or a dedicated unit test file) — unit coverage for JOIN_ACK secondary population + leaf secondary registration + `masterE2EKeys` post-failover.

---

### Task 1: Primary master stamps the secondary identity into JOIN_ACK

**Files:**
- Modify: `main/src/mesh/Mesh.h` (`enrollPeer` overload decl), `main/src/mesh/Mesh.cpp` (JOIN_ACK build)
- Modify: `main/src/adapter/serial/SerialAdapter.cpp` (pass server-provided secondary fields through)
- Modify: `tests/unit/test_mesh_logic.cpp` (unit test)

**Interfaces:**
- Produces:
  - `void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac, const uint8_t* secondaryPubKey32)` — 4-arg overload; stamps `ack.secondary_master_mac`/`ack.secondary_public_key` when the secondary args are non-null (else zero, i.e. no secondary). Existing 2-arg `enrollPeer(mac, publicKey32)` delegates with `nullptr, nullptr` (preserves current single-master callers).

- [ ] **Step 1: Write the failing unit test**

Add to `tests/unit/test_mesh_logic.cpp` (mirror the existing master JOIN_ACK test that inspects the broadcast frame via the esp_now mock — read it for the idiom):

```cpp
TEST_F(JoinAckRelayTest, EnrollPeerStampsSecondaryIdentityIntoJoinAck) {
  Mesh master = makeMasterNode();
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  uint8_t leafPub[32]; for (int i = 0; i < 32; ++i) leafPub[i] = static_cast<uint8_t>(i + 1);
  const uint8_t sec[6] = {0x02, 0, 0, 0, 0, 0x02};
  uint8_t secPub[32]; for (int i = 0; i < 32; ++i) secPub[i] = static_cast<uint8_t>(0x40 + i);

  resetEspNowMock();
  master.enrollPeer(leaf, leafPub, sec, secPub);

  // JOIN_ACK is broadcast; find it in the mock's sent frames.
  mesh_message ack = lastEspNowBroadcastOfType(MESH_TYPE_JOIN_ACK); // fixture/mock query
  ASSERT_TRUE(sawBroadcastOfType(MESH_TYPE_JOIN_ACK));
  EXPECT_EQ(0, memcmp(ack.secondary_master_mac, sec, 6));
  EXPECT_EQ(0, memcmp(ack.secondary_public_key, secPub, 32));
}

TEST_F(JoinAckRelayTest, EnrollPeerTwoArgLeavesSecondaryZero) {
  Mesh master = makeMasterNode();
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  uint8_t leafPub[32] = {9};
  resetEspNowMock();
  master.enrollPeer(leaf, leafPub); // 2-arg: no secondary
  mesh_message ack = lastEspNowBroadcastOfType(MESH_TYPE_JOIN_ACK);
  uint8_t zero6[6] = {}, zero32[32] = {};
  EXPECT_EQ(0, memcmp(ack.secondary_master_mac, zero6, 6));
  EXPECT_EQ(0, memcmp(ack.secondary_public_key, zero32, 32));
}
```

Adapt `makeMasterNode`/`lastEspNowBroadcastOfType`/`sawBroadcastOfType` to the fixture + esp_now mock helpers that actually exist (read them first; the mock records sent frames — query by message_type / broadcast dest).

- [ ] **Step 2: Run RED**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --target test_mesh_logic --parallel && ctest --test-dir tests/build -R EnrollPeerStampsSecondaryIdentityIntoJoinAck --output-on-failure
```
Expected: FAIL — 4-arg `enrollPeer` not declared / secondary fields zero.

- [ ] **Step 3: Implement**

In `Mesh.h`, add the overload next to the existing decl:

```cpp
void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32);
void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                const uint8_t* secondaryPubKey32);
```

In `Mesh.cpp`, make the existing body the 4-arg version and delegate from the 2-arg:

```cpp
void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32) {
  enrollPeer(mac, publicKey32, nullptr, nullptr);
}

void Mesh::enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                      const uint8_t* secondaryPubKey32) {
  // ... existing registerPeerWithKey + JOIN_ACK build ...
  // after ack.enrollment_public_key is set, before broadcast:
  if (secondaryMac && secondaryPubKey32) {
    memcpy(ack.secondary_master_mac, secondaryMac, 6);
    memcpy(ack.secondary_public_key, secondaryPubKey32, 32);
  }
  // ... existing broadcast ...
}
```

In `SerialAdapter.cpp` JOIN_ACK handler (`~:241-260`): read the server-provided secondary fields and pass them through. Only pass them if non-zero (server included a secondary):

```cpp
    if (hasKey) {
      lattice::mesh::Mesh* meshInstance = lattice::mesh::Mesh::getInstance();
      if (meshInstance) {
        bool hasSecondary = false;
        for (int i = 0; i < 6; ++i)
          if (msg.secondary_master_mac[i]) { hasSecondary = true; break; }
        if (hasSecondary)
          meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key,
                                   msg.secondary_master_mac, msg.secondary_public_key);
        else
          meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key);
      }
    }
```

- [ ] **Step 4: GREEN + full suite; clang-format; commit**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp main/src/adapter/serial/SerialAdapter.cpp
git add main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp main/src/adapter/serial/SerialAdapter.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat: primary master stamps server-provided secondary identity into JOIN_ACK"
```
Expected: 178 pass / 0 disabled.

---

### Task 2: Leaf registers the secondary master at enrollment

**Files:**
- Modify: `main/src/mesh/Enrollment.h`/`Enrollment.cpp` (`processJoinAck`)
- Modify: `tests/unit/test_mesh_logic.cpp` or `test_enrollment`-style unit test

**Interfaces:**
- Consumes: `RegisterPeerFn registerFn` (already passed to `processJoinAck`); `EepromManager::saveKnownMasterMacSecondary`.
- Produces: after a JOIN_ACK carrying non-zero `secondary_*`, the leaf has the secondary registered as a `PeerRegistry` peer (mac+pubkey, persisted) and `hasMasterMacSecondary`/`knownMasterMacSecondary` set+persisted.

- [ ] **Step 1: Write the failing test**

Add a leaf-side unit test (mirror the existing `processJoinAck` test that checks the primary is registered — read it for how the leaf Mesh/Enrollment is built and how `registerFn` wires into `registerPeerWithKey`):

```cpp
TEST_F(JoinAckRelayTest, ProcessJoinAckRegistersSecondaryMasterAndKeys) {
  Mesh leaf = makeIntermediateNode(); // non-master, not yet enrolled with anyone (fixture)
  // Build a JOIN_ACK addressed to the leaf, carrying primary + secondary identities.
  uint8_t leafPriv[32], leafPub[32], primPriv[32], primPub[32], secPriv[32], secPub[32];
  crypto::generateKeypair(leafPriv, leafPub);
  crypto::generateKeypair(primPriv, primPub);
  crypto::generateKeypair(secPriv, secPub);
  // (fixture: install leafPriv/leafPub as the leaf's device keypair)
  const uint8_t primMac[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t secMac[6]  = {0x02, 0, 0, 0, 0, 0x02};
  mesh_message ack = {};
  ack.proto_version = PROTO_VERSION;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.origin_mac_address, primMac, 6);          // primary sent it
  memcpy(ack.target_mac_address, leaf.testDeviceMac(), 6);
  memcpy(ack.data, leafPub, 4);                        // fingerprint of the leaf's pubkey
  memcpy(ack.enrollment_public_key, primPub, 32);      // primary pubkey
  memcpy(ack.secondary_master_mac, secMac, 6);
  memcpy(ack.secondary_public_key, secPub, 32);

  leaf.processJoinAck(ack); // routes to enrollment.processJoinAck with the registerFn

  // Secondary registered as a routable/keyable peer with its pubkey:
  PeerInfo* sec = leaf.peers.find(secMac);
  ASSERT_NE(sec, nullptr);
  EXPECT_EQ(0, memcmp(sec->publicKey, secPub, 32));
  // Secondary TOFU state set:
  EXPECT_TRUE(leaf.enrollment.hasMasterMacSecondary);
  EXPECT_EQ(0, memcmp(leaf.enrollment.knownMasterMacSecondary, secMac, 6));
  // And post-failover masterE2EKeys resolves against the secondary:
  leaf.enrollment.hasMasterMac = true; // enrolled with primary
  memcpy(leaf.currentMaster.mac, secMac, 6); // simulate adoption
  leaf.currentMaster.distance = 1;
  const uint8_t *kUp, *kDown;
  EXPECT_TRUE(leaf.masterE2EKeys(&kUp, &kDown)) << "keys derivable against the secondary post-failover";
}
```

Adapt to the fixture's real keypair-install + `processJoinAck` reachability (the map shows `Mesh::processJoinAck(const mesh_message&)` is public under UNIT_TEST). If the fixture requires the leaf to already be enrolled for `processJoinAck` to register a peer (TOFU gating), set that up as the existing primary-registration test does.

- [ ] **Step 2: Run RED** — secondary not registered / `hasMasterMacSecondary` false.

- [ ] **Step 3: Implement**

In `Enrollment::processJoinAck` (`Enrollment.cpp:92-134`), after the primary is registered via `registerFn(msg.origin_mac_address, msg.enrollment_public_key)` and the enrolled flag/primary-TOFU handling, add secondary handling:

```cpp
  // Dual-master (spec §5): if the server included a secondary master, register it
  // as a peer (persists mac+pubkey, so masterE2EKeys can derive against it after
  // failover) and record it as the secondary for beacon adoption. Guarded on a
  // non-zero secondary MAC.
  bool hasSecondary = false;
  for (int i = 0; i < 6; ++i)
    if (msg.secondary_master_mac[i]) { hasSecondary = true; break; }
  if (hasSecondary) {
    registerFn(msg.secondary_master_mac, msg.secondary_public_key);
    if (!hasMasterMacSecondary) {
      memcpy(knownMasterMacSecondary, msg.secondary_master_mac, 6);
      hasMasterMacSecondary = true;
      EepromManager::getInstance().saveKnownMasterMacSecondary(knownMasterMacSecondary);
    }
  }
```

Ensure `EepromManager.h` is included in `Enrollment.cpp` (it already is — `saveEnrolledFlag`/`saveKnownMasterMac` are used).

Note: `registerFn` is `Mesh::registerPeerWithKey(..., allowRekey=false)` — first-contact registration of the secondary is allowed; an established key is not replaced (security gating preserved). Confirm the secondary being a distinct new MAC means `append` (not rekey).

- [ ] **Step 4: GREEN + full suite; clang-format; commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Enrollment.cpp main/src/mesh/Enrollment.h
git add main/src/mesh/Enrollment.cpp main/src/mesh/Enrollment.h tests/unit/test_mesh_logic.cpp
git commit -m "feat: leaf registers secondary master (mac+pubkey) from JOIN_ACK for post-failover keying"
```
Expected: 179 pass / 0 disabled.

---

### Task 3: Harness two-master server-sync + data-failover e2e (gap #8 executable spec)

**Files:**
- Modify: `tests/e2e/harness/FakeHub.h`/`FakeHub.cpp` (`approveEnrollment` secondary overload)
- Modify: `tests/e2e/scenarios/test_dual_master_e2e.cpp` (new data-failover test)

**Interfaces:**
- Consumes: Tasks 1-2 (JOIN_ACK carries secondary; leaf registers it); `Mesh::enrollPeer` (to sync a leaf's pubkey to the secondary master); dual-master adoption (existing).
- Produces: an e2e test proving post-failover uplink DECRYPTS at the secondary (not just route adoption).

- [ ] **Step 1: Add the FakeHub secondary-aware approval**

In `FakeHub.h`/`FakeHub.cpp`, add an overload so a test can drive the server-provides-secondary flow:

```cpp
// approve enrollment AND tell the node about the secondary master (server-designated).
void approveEnrollment(const uint8_t* nodeMac, const uint8_t* nodePubKey32,
                       const uint8_t* secondaryMac, const uint8_t* secondaryPubKey32);
```

Its body mirrors the existing `approveEnrollment` but also sets `ack.secondary_master_mac`/`ack.secondary_public_key` before it injects/sends the JOIN_ACK frame into the master's serial path. Keep the existing 2-arg overload delegating with zero secondary.

- [ ] **Step 2: Write the failing data-failover e2e test**

In `test_dual_master_e2e.cpp`, add (reuse `DualMasterTest`/`bringUpSecondary`; the header comment already flags data-failover as untested):

```cpp
TEST_F(DualMasterTest, UplinkReachesSecondaryMasterAfterFailover) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  auto* master2 = bringUpSecondary(sensor); // dual mode on; master2 linked to sensor + primary

  // Read master2's pubkey (server knows both masters' identities).
  uint8_t m2Pub[32];
  master2->with([&](Mesh& m, Adapter*) { memcpy(m2Pub, m.enrollment.getPublicKey(), 32); return 0; });

  // Enroll the sensor with the PRIMARY, server designating master2 as the secondary.
  const mesh_message* req = hub->enrollmentFrom(sensor->mac());
  ASSERT_NE(req, nullptr);
  hub->approveEnrollment(sensor->mac(), req->enrollment_public_key, master2->mac(), m2Pub);
  runPolled(5000);
  ASSERT_TRUE(sensor->with([&](Mesh& m, Adapter*) { return m.isEnrolled(); }));

  // Server sync: teach master2 the sensor's pubkey (so master2 can open its uplink).
  uint8_t sensorPub[32];
  sensor->with([&](Mesh& m, Adapter*) { memcpy(sensorPub, m.enrollment.getPublicKey(), 32); return 0; });
  master2->with([&](Mesh& m, Adapter*) { m.enrollPeer(sensor->mac(), sensorPub); return 0; });

  // Attach a hub to master2 so we can observe what reaches it.
  FakeHub hub2(master2);

  // Primary goes silent → sensor fails over to master2.
  world.bus.unlink(master, sensor);
  runPolled(STALE_MASTER_THRESHOLD_MS + 4000);
  ASSERT_TRUE(sensor->with([&](Mesh& m, Adapter*) {
    return memcmp(m.currentMaster.mac, master2->mac(), 6) == 0;
  }));

  // Sensor sends PIR data; it must arrive DECRYPTED at master2's hub.
  size_t before = hub2.adapterDataFromOrigin(sensor->mac()).size();
  sensor->simulatePirMotion();
  runPolled(4000);
  hub2.poll();
  auto frames = hub2.adapterDataFromOrigin(sensor->mac());
  ASSERT_GT(frames.size(), before) << "post-failover uplink reached the secondary";
  // FakeHub delivers what the master OPENED — so arrival proves master2 decrypted it
  // with the sensor's k_up derived against the (sensor, master2) ECDH pair.
}
```

Adapt to the harness: confirm `FakeHub` can be constructed on `master2` mid-test and that `adapterDataFromOrigin`/`poll` surface the master's opened uplink (read `FakeHub` for how it records adapter data — it decodes what the master bridged to serial, i.e. post-open). If `bringUpSecondary` enrolls the sensor before you can inject the secondary identity, restructure so the `approveEnrollment` with secondary args is the enrollment that registers the sensor (the sensor must learn the secondary at enrollment time).

- [ ] **Step 3: Run RED**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build -R UplinkReachesSecondaryMasterAfterFailover --output-on-failure
```
Expected: FAIL at the final `ASSERT_GT` — pre-Task-1/2 the sensor never learned master2's key, so its post-failover uplink seal uses no/ wrong key and never decrypts at master2. (Since Tasks 1-2 landed, the failing driver here is the harness wiring; confirm the test genuinely exercises decryption — if it passes trivially, verify master2 actually opens the frame rather than the harness surfacing plaintext.)

- [ ] **Step 4: Make it pass**

With Tasks 1-2 in place, the sensor learns master2 at enrollment and seals post-failover uplink with the (sensor, master2) `k_up`; master2 opens it with `peerE2EKeys(sensor)` (sensor synced in Step 2). If the test fails, debug the real wiring (harness hub-on-master2, server-sync call, adoption timing) — do NOT weaken the decryption assertion. The test asserting arrival at master2's hub == master2 successfully decrypted (FakeHub surfaces opened payloads).

- [ ] **Step 5: Full suite; clang-format (no main/src change expected here — harness/tests only); commit**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
git add tests/e2e/harness/FakeHub.h tests/e2e/harness/FakeHub.cpp tests/e2e/scenarios/test_dual_master_e2e.cpp
git commit -m "test: dual-master data failover e2e — uplink decrypts at secondary (closes gap #8)"
```
Expected: 180 pass / 0 disabled.

---

### Task 4: Finish — full verification + gap doc + PR

**Files:**
- Modify: `docs/design-gaps/multihop-data-uplink.md` (mark #8 closed)

- [ ] **Step 1: Clean-from-scratch build + suite**

```bash
rm -rf tests/build && cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: all pass, 0 disabled.

- [ ] **Step 2: clang-format-18 gate (main/src)**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
find main/src \( -name '*.cpp' -o -name '*.h' \) ! -path '*/nanopb/*' ! -name 'mesh.pb.h' ! -name 'mesh.pb.c' | xargs "$CF" --style=file --dry-run --Werror
```
Expected: exit 0.

- [ ] **Step 3: Mark gap #8 closed**

In `docs/design-gaps/multihop-data-uplink.md`, update the "Related gap: dual-master data failover" section (and any status line) to note #8 is closed in Phase 4: the leaf registers the secondary master's key at enrollment (server-provided via JOIN_ACK), so post-failover uplink seals with the secondary's `k_up` and decrypts at the secondary (which the server syncs each leaf's pubkey to).

Commit:

```bash
git add docs/design-gaps/multihop-data-uplink.md
git commit -m "docs: mark dual-master data failover (gap #8) closed in Phase 4"
```

- [ ] **Step 4: PR via superpowers:finishing-a-development-branch**

Single lattice-nodes PR (no protocol/submodule change). PR body: closes gap #8; JOIN_ACK carries server-provided secondary identity; leaf registers the secondary as a persisted peer (no EEPROM/wire change); secondary opens post-failover uplink via server-synced leaf pubkey; new data-failover e2e. Note: gh active account must be `superbrobenji` (ADMIN).

---

## Deferred (explicitly NOT in Phase 4)

- **A real server sync protocol/opcode** for pushing leaf pubkeys to the secondary master: modeled in the harness via the existing `enrollPeer` path; a production server-sync opcode (if the firmware ever needs to drive it without per-leaf enroll frames) is out of scope. The mechanism (server → secondary master `enrollPeer(leaf, pubkey)`) already exists on the serial path.
- **`transmitCore` AAD-bound `target_mac` rewrite on relayed sealed frames** (carried from Phases 1-3): only bites when a relay's `currentMaster` differs from the origin's target — possible in a multi-master + multi-hop combination. If the dual-master e2e surfaces it, note it; otherwise it remains a documented follow-up.
- **Sticky-low `currentMaster.distance`** and other carried minors from earlier phases.
- **Secondary-of-secondary / >2 masters:** out of scope; the design handles exactly one secondary.
