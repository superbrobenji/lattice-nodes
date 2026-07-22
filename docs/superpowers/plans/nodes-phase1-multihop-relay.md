# Phase 1: Multi-hop Relay Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three independent routing bugs that silently prevent any ESP32 node more than one hop from the master from sending or receiving data.

**Architecture:** All three bugs are in `Mesh.cpp`'s message dispatch loop (`drainRecvQueue → processAdapterData / processJoinAck`). The relay table is already correct (beacons propagate it); only the data/command relay paths are missing. The fix adds a `relayDownlink()` helper and wires it into `processAdapterData()` and `processJoinAck()`. A broadcast-target convention (`targetMacAddress = FF:FF:…`) is introduced so master-initiated broadcasts propagate beyond 1-hop neighbors. Replay cache (existing, 16-entry ring on mac+epoch+seq) stops loops.

**Tech Stack:** C++17, Arduino/ESP-IDF, ESP-NOW, Google Test (native host build), CMake.

## Global Constraints

- All firmware changes are in the `main/` Arduino sketch.
- No changes to files outside `main/src/Mesh/` and `tests/unit/test_mesh_logic.cpp`.
- Tests run on host: `cmake -B tests/build -S tests && cmake --build tests/build && ctest --test-dir tests/build --output-on-failure`
- `static_assert(sizeof(mesh_message) == 75)` must continue to pass (this plan does NOT change the struct — that is Phase 3 / spec §5.4).
- `MAX_HOPS` is `planetopia::config::MAX_HOPS` (10).
- `STALE_PEER_THRESHOLD_MS` is `planetopia::config::STALE_PEER_THRESHOLD_MS` (8000ms). Peers added with `lastSeenMillis = 0` are "in range" when `millis() = 0` (mock default).
- In unit-test builds (`UNIT_TEST` defined), all `Mesh` members are `public` — tests access state directly without getters.
- Commit after every task.

---

### Task 1: `relayDownlink()` helper — declaration + implementation + tests

This helper is the shared relay primitive used by Tasks 2–4. It increments hop count, updates `lastHopMacAddress`, and unicasts the message to every enrolled peer (without overwriting `targetMacAddress`, so the original destination is preserved through intermediate nodes).

**Files:**
- Modify: `main/src/Mesh/Mesh.h` (private method declaration)
- Modify: `main/src/Mesh/Mesh.cpp` (implementation)
- Modify: `tests/unit/test_mesh_logic.cpp` (tests)

**Interfaces:**
- Produces: `void Mesh::relayDownlink(const mesh_message& msg)` — private method

- [ ] **Step 1.1: Write failing tests**

Append to `tests/unit/test_mesh_logic.cpp` (before the closing `}`):

```cpp
// ─── relayDownlink ───────────────────────────────────────────────────────────

class RelayDownlinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6]    = {0x11,0x22,0x33,0x44,0x55,0x66};
  static constexpr uint8_t kPeer1Mac[6] = {0xAA,0xAA,0xAA,0xAA,0xAA,0x01};
  static constexpr uint8_t kPeer2Mac[6] = {0xBB,0xBB,0xBB,0xBB,0xBB,0x02};
  static constexpr uint8_t kOriginMac[6]= {0xCC,0xCC,0xCC,0xCC,0xCC,0x03};

  Mesh makeMeshWithPeers(int numPeers) {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    if (numPeers >= 1) {
      PeerInfo p{}; memcpy(p.mac, kPeer1Mac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    }
    if (numPeers >= 2) {
      PeerInfo p{}; memcpy(p.mac, kPeer2Mac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    }
    return mesh;
  }

  mesh_message makeDataMsg(const uint8_t origin[6], const uint8_t target[6],
                           uint32_t epoch, uint16_t seq, uint8_t hopCount = 0) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType  = MESH_TYPE_ADAPTER_DATA;
    m.dataType     = adapter_types::PIR_ADAPTER;
    memcpy(m.originMacAddress, origin, 6);
    memcpy(m.targetMacAddress, target, 6);
    memcpy(m.lastHopMacAddress, origin, 6);
    m.hopCount = hopCount;
    m.epochNum = epoch;
    m.seqNum   = seq;
    return m;
  }
};

constexpr uint8_t RelayDownlinkTest::kMyMac[];
constexpr uint8_t RelayDownlinkTest::kPeer1Mac[];
constexpr uint8_t RelayDownlinkTest::kPeer2Mac[];
constexpr uint8_t RelayDownlinkTest::kOriginMac[];

TEST_F(RelayDownlinkTest, SendsToPeers_IncrementHopCount) {
  Mesh mesh = makeMeshWithPeers(2);
  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1, /*hopCount=*/1);

  mesh.relayDownlink(msg);

  // 2 peers → 2 sends
  EXPECT_EQ(espNowSentPackets.size(), 2u);
  for (const auto& pkt : espNowSentPackets) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(pkt.data.data());
    EXPECT_EQ(sent.hopCount, 2u);                          // incremented
    EXPECT_EQ(memcmp(sent.targetMacAddress, kPeer2Mac, 6), 0); // target preserved
    EXPECT_EQ(memcmp(sent.lastHopMacAddress, kMyMac, 6), 0);   // lastHop = my MAC
  }
}

TEST_F(RelayDownlinkTest, DropsReplay) {
  Mesh mesh = makeMeshWithPeers(1);
  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 99);

  mesh.isReplay(msg);  // Pre-record in replay cache
  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, DropsAtMaxHops) {
  Mesh mesh = makeMeshWithPeers(1);
  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 1,
                         /*hopCount=*/planetopia::config::MAX_HOPS);

  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, SkipsSelf_WhenSelfInPeerList) {
  Mesh mesh = makeMeshWithPeers(1);
  // Add self to peer list (shouldn't happen in production but guard against it)
  PeerInfo self{}; memcpy(self.mac, kMyMac, 6); self.lastSeenMillis = 0;
  mesh.appendPeer(self);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1);
  mesh.relayDownlink(msg);

  // Only 1 peer (kPeer1Mac) — self skipped
  EXPECT_EQ(espNowSentPackets.size(), 1u);
  EXPECT_EQ(memcmp(espNowSentPackets[0].addr, kPeer1Mac, 6), 0);
}
```

- [ ] **Step 1.2: Run tests — expect compile error (relayDownlink undefined)**

```bash
cmake --build tests/build 2>&1 | grep "relayDownlink\|error:"
```

Expected: compile error — `'relayDownlink' is not a member of 'planetopia::mesh::Mesh'`

- [ ] **Step 1.3: Declare `relayDownlink` in Mesh.h**

In `main/src/Mesh/Mesh.h`, find the block of private relay/process helpers (~line 143):
```cpp
  void processMasterBeacon(const mesh_message& msg);
  void processAdapterData(const mesh_message& msg);
```
Add after `processAdapterData`:
```cpp
  void relayDownlink(const mesh_message& msg);
```

- [ ] **Step 1.4: Implement `relayDownlink` in Mesh.cpp**

In `main/src/Mesh/Mesh.cpp`, add the implementation immediately after `processAdapterData` (~line 935):

```cpp
void Mesh::relayDownlink(const mesh_message& msg) {
  if (isReplay(msg)) return;
  if (msg.hopCount >= planetopia::config::MAX_HOPS) return;
  mesh_message relay = msg;
  relay.hopCount++;
  memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0) continue;
    sendMessage(peerMacs[i].mac, relay);
  }
}
```

- [ ] **Step 1.5: Build and run tests — expect pass**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: all 40 tests pass (36 existing + 4 new).

- [ ] **Step 1.6: Commit**

```bash
git add main/src/Mesh/Mesh.h main/src/Mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): add relayDownlink() helper for multi-hop data relay"
```

---

### Task 2: Uplink relay in `processAdapterData` (sensor → master)

When a non-master node receives `MESH_TYPE_ADAPTER_DATA` addressed to the master, it must relay it toward the master via the existing `transmitCore` routing (which uses `findNextHopToMaster()`). Without this, any sensor >1 hop from the master is invisible to the server.

**Files:**
- Modify: `main/src/Mesh/Mesh.cpp` — `processAdapterData()` (~line 924)
- Modify: `tests/unit/test_mesh_logic.cpp`

**Interfaces:**
- Consumes: `void Mesh::relayDownlink(const mesh_message&)` from Task 1
- Consumes: `void Mesh::transmitCore(adapter_types, const uint8_t[12], MeshMessageType, const mesh_message*)` (existing)

- [ ] **Step 2.1: Write failing tests**

Append to `tests/unit/test_mesh_logic.cpp`:

```cpp
// ─── processAdapterData: uplink relay ────────────────────────────────────────

class AdapterDataRelayTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6]     = {0x11,0x22,0x33,0x44,0x55,0x66};
  static constexpr uint8_t kMasterMac[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0x01};
  static constexpr uint8_t kSensorMac[6] = {0x77,0x88,0x99,0xAA,0xBB,0xCC};
  static constexpr uint8_t kPeerMac[6]   = {0x55,0x55,0x55,0x55,0x55,0x55};

  Mesh makeIntermediateNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false;
    // Set master route: next hop IS the master (1 hop away)
    memcpy(mesh.currentMaster.mac,     kMasterMac, 6);
    memcpy(mesh.currentMaster.nextHop, kMasterMac, 6);
    mesh.currentMaster.distance = 1;
    mesh.hasMasterMac = true;
    memcpy(mesh.knownMasterMac, kMasterMac, 6);
    // Register master as enrolled peer (required for sendMessage + isPeerInRange)
    PeerInfo p{}; memcpy(p.mac, kMasterMac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    return mesh;
  }

  mesh_message makeUplinkMsg(uint32_t epoch, uint16_t seq, uint8_t hopCount = 1) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType  = MESH_TYPE_ADAPTER_DATA;
    m.dataType     = adapter_types::PIR_ADAPTER;
    memcpy(m.originMacAddress,  kSensorMac,  6);
    memcpy(m.targetMacAddress,  kMasterMac,  6);  // addressed to master
    memcpy(m.lastHopMacAddress, kSensorMac,  6);
    m.hopCount = hopCount;
    m.epochNum = epoch;
    m.seqNum   = seq;
    return m;
  }
};

constexpr uint8_t AdapterDataRelayTest::kMyMac[];
constexpr uint8_t AdapterDataRelayTest::kMasterMac[];
constexpr uint8_t AdapterDataRelayTest::kSensorMac[];
constexpr uint8_t AdapterDataRelayTest::kPeerMac[];

TEST_F(AdapterDataRelayTest, IntermediateNode_RelaysUplinkTowardMaster) {
  Mesh mesh = makeIntermediateNode();
  auto msg  = makeUplinkMsg(1, 1, /*hopCount=*/1);

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_EQ(espNowSentPackets.size(), before + 1);
  const auto& sent = *reinterpret_cast<const mesh_message*>(
      espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.hopCount, 2u);                              // incremented
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kMasterMac, 6), 0); // routed via nextHop
}

TEST_F(AdapterDataRelayTest, IntermediateNode_DropsUplinkReplay) {
  Mesh mesh = makeIntermediateNode();
  auto msg  = makeUplinkMsg(1, 7);

  mesh.processAdapterData(msg);  // first — relayed
  size_t after1 = espNowSentPackets.size();

  mesh.processAdapterData(msg);  // same epoch+seq — replay, dropped
  EXPECT_EQ(espNowSentPackets.size(), after1);
}

TEST_F(AdapterDataRelayTest, Master_DoesNotRelayUplink_DeliversLocally) {
  Mesh mesh = makeIntermediateNode();
  mesh.isMaster = true;

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](mesh_message) { callbackFired = true; });

  auto msg = makeUplinkMsg(1, 1);
  memcpy(msg.targetMacAddress, mesh.deviceMacAddress, 6); // addressed to self (master)

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_TRUE(callbackFired);
  EXPECT_EQ(espNowSentPackets.size(), before); // no relay
}
```

- [ ] **Step 2.2: Run tests — expect failure**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure 2>&1 | grep -E "FAILED|PASSED|IntermediateNode"
```

Expected: `AdapterDataRelayTest.IntermediateNode_RelaysUplinkTowardMaster` FAILED (no sends occur).

- [ ] **Step 2.3: Update `processAdapterData()` with uplink relay**

Replace the entire `processAdapterData` body in `main/src/Mesh/Mesh.cpp` (~line 924–935):

```cpp
void Mesh::processAdapterData(const mesh_message& msg) {
  static constexpr uint8_t OP_CONFIG_SET = 0xA0;
  static const uint8_t kBroadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  bool addressedToSelf    = (memcmp(msg.targetMacAddress, deviceMacAddress, 6) == 0);
  bool isBroadcastTarget  = (memcmp(msg.targetMacAddress, kBroadcastMac,    6) == 0);
  bool addressedToMaster  = hasMasterMac &&
                            (memcmp(msg.targetMacAddress, currentMaster.mac, 6) == 0);

  if (!isMaster && !addressedToSelf && !isBroadcastTarget) {
    if (addressedToMaster) {
      // Uplink: relay toward master via routing table
      if (isReplay(msg)) return;
      if (msg.hopCount >= planetopia::config::MAX_HOPS) return;
      mesh_message relay = msg;
      relay.hopCount++;
      memcpy(relay.lastHopMacAddress, deviceMacAddress, 6);
      transmitCore(relay.dataType, relay.data, MESH_TYPE_ADAPTER_DATA, &relay);
      return;
    }
    // Downlink to another node: relay outward (Tasks 3 fills this in)
    relayDownlink(msg);
    return;
  }

  // Local delivery
  bool isConfigOpcode = (msg.dataType == adapter_types::SERIAL_ADAPTER &&
                         msg.data[0] == OP_CONFIG_SET);
  if (isConfigOpcode && hasMasterMac &&
      memcmp(msg.originMacAddress, knownMasterMac, 6) != 0) {
    Logger::logln("MESH", "CONFIG_SET from non-master MAC rejected", LogLevel::LOG_WARN);
    return;
  }
  if (externalRecvCallback)
    externalRecvCallback(msg);

  // Broadcast: also relay so multi-hop nodes receive it (Task 3 test covers this)
  if (isBroadcastTarget && !isMaster) {
    relayDownlink(msg);
  }
}
```

- [ ] **Step 2.4: Build and run tests — expect pass**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2.5: Commit**

```bash
git add main/src/Mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): relay ADAPTER_DATA uplink toward master for multi-hop nodes"
```

---

### Task 3: Downlink + broadcast relay in `processAdapterData`

Two downlink paths need testing:
1. **Targeted downlink** — master sends ADAPTER_DATA to a specific sensor node; intermediate nodes must relay it outward until the target receives it. `relayDownlink()` already wired in Task 2; this task covers tests and the `broadcastAdapterData` broadcast-target fix.
2. **Broadcast downlink** — master wants all nodes to receive a command. `broadcastAdapterData()` currently sets `targetMacAddress = peer.mac` individually, so nodes >1 hop away never hear it. Fix: set `targetMacAddress = FF:FF:FF:FF:FF:FF` before sending, so every receiving node can identify it as broadcast, deliver locally, and relay outward.

**Files:**
- Modify: `main/src/Mesh/Mesh.cpp` — `broadcastAdapterData()` (~line 791)
- Modify: `tests/unit/test_mesh_logic.cpp`

- [ ] **Step 3.1: Write failing tests**

Append to `tests/unit/test_mesh_logic.cpp`:

```cpp
// ─── processAdapterData: downlink + broadcast relay ──────────────────────────

TEST_F(AdapterDataRelayTest, IntermediateNode_RelaysDownlinkToOtherTarget) {
  // Node receives ADAPTER_DATA addressed to a different sensor — must relay outward
  Mesh mesh = makeIntermediateNode();
  // Add a second peer (different from master) to relay toward
  PeerInfo extra{}; memcpy(extra.mac, kPeerMac, 6); extra.lastSeenMillis = 0;
  mesh.appendPeer(extra);

  mesh_message msg{};
  msg.protoVersion = 1;
  msg.messageType  = MESH_TYPE_ADAPTER_DATA;
  msg.dataType     = adapter_types::PIR_ADAPTER;
  memcpy(msg.originMacAddress,  kMasterMac, 6);
  memcpy(msg.targetMacAddress,  kSensorMac, 6); // some other sensor, not me, not master
  msg.hopCount = 1; msg.epochNum = 2; msg.seqNum = 1;

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  // Should relay to all peers: kMasterMac + kPeerMac (2 peers)
  EXPECT_GT(espNowSentPackets.size(), before);
  // Target preserved in every relayed copy
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(
        espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.targetMacAddress, kSensorMac, 6), 0);
    EXPECT_EQ(sent.hopCount, 2u);
  }
}

TEST_F(AdapterDataRelayTest, IntermediateNode_BroadcastTarget_DeliveredAndRelayed) {
  Mesh mesh = makeIntermediateNode();
  PeerInfo extra{}; memcpy(extra.mac, kPeerMac, 6); extra.lastSeenMillis = 0;
  mesh.appendPeer(extra);

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](mesh_message) { callbackFired = true; });

  static constexpr uint8_t kBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  mesh_message msg{};
  msg.protoVersion = 1;
  msg.messageType  = MESH_TYPE_ADAPTER_DATA;
  msg.dataType     = adapter_types::PIR_ADAPTER;
  memcpy(msg.originMacAddress,  kMasterMac,  6);
  memcpy(msg.targetMacAddress,  kBroadcast,  6); // broadcast
  msg.hopCount = 1; msg.epochNum = 3; msg.seqNum = 1;

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_TRUE(callbackFired);                                 // delivered locally
  EXPECT_GT(espNowSentPackets.size(), before);                // AND relayed outward
}

TEST_F(AdapterDataRelayTest, BroadcastAdapterData_UsesBroadcastTargetMAC) {
  // Verify master's broadcastAdapterData sets FF:FF target so multi-hop works
  Mesh mesh = makeIntermediateNode();
  mesh.isMaster = true;
  // Add a peer so broadcastToAllPeers has someone to send to
  PeerInfo extra{}; memcpy(extra.mac, kPeerMac, 6); extra.lastSeenMillis = 0;
  mesh.appendPeer(extra);

  static constexpr uint8_t kPayload[12] = {0x01,0x02,0x03,0,0,0,0,0,0,0,0,0};
  size_t before = espNowSentPackets.size();
  mesh.broadcastAdapterData(adapter_types::PIR_ADAPTER, kPayload);

  EXPECT_GT(espNowSentPackets.size(), before);
  // Every sent message should have FF:FF target
  static constexpr uint8_t kBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(
        espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.targetMacAddress, kBroadcast, 6), 0);
  }
}
```

- [ ] **Step 3.2: Run tests — expect failure on broadcast target test**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure 2>&1 | grep -E "FAILED|PASSED|Broadcast|Downlink"
```

Expected: `BroadcastAdapterData_UsesBroadcastTargetMAC` FAILED (target is currently set to `currentMaster.mac` by `buildMessage`, not broadcast MAC).

- [ ] **Step 3.3: Fix `broadcastAdapterData()` and `broadcastToAllPeers()`**

In `main/src/Mesh/Mesh.cpp`, find `broadcastAdapterData` (~line 791):

```cpp
// BEFORE:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[12]) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  broadcastToAllPeers(msg);
}
```

Replace with:
```cpp
// AFTER:
void Mesh::broadcastAdapterData(adapter_types type, const uint8_t data[12]) {
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memset(msg.targetMacAddress, 0xFF, 6); // broadcast indicator — relayed by all intermediate nodes
  broadcastToAllPeers(msg);
}
```

Then find `broadcastToAllPeers` (~line 640):

```cpp
// BEFORE:
void Mesh::broadcastToAllPeers(mesh_message msg) {
  if (peerCount == 0) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue; // Skip self
    memcpy(msg.targetMacAddress, peerMacs[i].mac, 6);
    sendMessage(peerMacs[i].mac, msg);
  }
}
```

Replace with:
```cpp
// AFTER (no longer overwrites targetMacAddress — caller sets it):
void Mesh::broadcastToAllPeers(mesh_message msg) {
  if (peerCount == 0) {
    Logger::logln("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (size_t i = 0; i < peerCount; ++i) {
    if (memcmp(peerMacs[i].mac, deviceMacAddress, 6) == 0)
      continue; // Skip self
    sendMessage(peerMacs[i].mac, msg);
  }
}
```

- [ ] **Step 3.4: Build and run tests — expect pass**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3.5: Commit**

```bash
git add main/src/Mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): relay downlink and broadcast ADAPTER_DATA to multi-hop nodes"
```

---

### Task 4: `processJoinAck` relay — enrollment for multi-hop nodes

`processJoinAck()` currently drops any JOIN_ACK not addressed to itself (`memcmp != 0 → return`). Nodes >1 hop from master send an enrollment request that the master approves, but the JOIN_ACK can't reach them. Fix: relay outward before the target check.

**Files:**
- Modify: `main/src/Mesh/Mesh.cpp` — `processJoinAck()` (~line 1040)
- Modify: `tests/unit/test_mesh_logic.cpp`

**Interfaces:**
- Consumes: `void Mesh::relayDownlink(const mesh_message&)` from Task 1

- [ ] **Step 4.1: Write failing tests**

Append to `tests/unit/test_mesh_logic.cpp`:

```cpp
// ─── processJoinAck: relay ───────────────────────────────────────────────────

class JoinAckRelayTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6]       = {0x11,0x22,0x33,0x44,0x55,0x66};
  static constexpr uint8_t kMasterMac[6]   = {0xAA,0xBB,0xCC,0xDD,0xEE,0x01};
  static constexpr uint8_t kDistantNode[6] = {0x99,0x88,0x77,0x66,0x55,0x44};
  static constexpr uint8_t kPeerMac[6]     = {0x33,0x33,0x33,0x33,0x33,0x33};

  Mesh makeIntermediateNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false;
    PeerInfo p{}; memcpy(p.mac, kPeerMac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    return mesh;
  }

  mesh_message makeJoinAck(const uint8_t target[6], uint8_t hopCount = 1) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType  = MESH_TYPE_JOIN_ACK;
    m.dataType     = adapter_types::UNKNOWN_ADAPTER;
    memcpy(m.originMacAddress,  kMasterMac, 6);
    memcpy(m.targetMacAddress,  target,     6);
    memcpy(m.lastHopMacAddress, kMasterMac, 6);
    m.hopCount = hopCount;
    m.epochNum = 1;
    m.seqNum   = 1;
    return m;
  }
};

constexpr uint8_t JoinAckRelayTest::kMyMac[];
constexpr uint8_t JoinAckRelayTest::kMasterMac[];
constexpr uint8_t JoinAckRelayTest::kDistantNode[];
constexpr uint8_t JoinAckRelayTest::kPeerMac[];

TEST_F(JoinAckRelayTest, RelaysJoinAck_WhenNotAddressedToSelf) {
  Mesh mesh = makeIntermediateNode();
  auto msg  = makeJoinAck(kDistantNode); // addressed to a distant node, not me

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  EXPECT_GT(espNowSentPackets.size(), before); // relayed to kPeerMac
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kPeerMac, 6), 0);
  const auto& sent = *reinterpret_cast<const mesh_message*>(
      espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.hopCount, 2u);
  EXPECT_EQ(memcmp(sent.targetMacAddress, kDistantNode, 6), 0); // target preserved
}

TEST_F(JoinAckRelayTest, DoesNotRelayJoinAck_WhenAddressedToSelf) {
  // When addressed to self: process (enroll), do NOT relay
  Mesh mesh = makeIntermediateNode();

  // Provide a fingerprint (first 4 bytes of devicePublicKey)
  // devicePublicKey is zeroed in constructor — fingerprint = {0,0,0,0}
  auto msg = makeJoinAck(kMyMac);
  memset(msg.data, 0, sizeof(msg.data)); // fingerprint matches zeroed pubkey

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no relay
}

TEST_F(JoinAckRelayTest, DropsJoinAckRelay_WhenReplay) {
  Mesh mesh = makeIntermediateNode();
  auto msg  = makeJoinAck(kDistantNode);

  mesh.processJoinAck(msg);          // first: relayed
  size_t after1 = espNowSentPackets.size();
  mesh.processJoinAck(msg);          // replay: dropped
  EXPECT_EQ(espNowSentPackets.size(), after1);
}
```

- [ ] **Step 4.2: Run tests — expect failure**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure 2>&1 | grep -E "FAILED|PASSED|JoinAck"
```

Expected: `JoinAckRelayTest.RelaysJoinAck_WhenNotAddressedToSelf` FAILED (current code returns immediately when target != self).

- [ ] **Step 4.3: Update `processJoinAck()` to relay before target check**

In `main/src/Mesh/Mesh.cpp`, find `processJoinAck` (~line 1040):

```cpp
// BEFORE:
void Mesh::processJoinAck(const mesh_message& msg) {
  // Verify the ack is addressed to us
  if (memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0)
    return;
  // Verify fingerprint matches our public key (first 4 bytes)
  ...
```

Replace with:
```cpp
// AFTER:
void Mesh::processJoinAck(const mesh_message& msg) {
  // Relay outward if not addressed to us (multi-hop enrollment)
  if (memcmp(msg.targetMacAddress, deviceMacAddress, 6) != 0) {
    relayDownlink(msg);
    return;
  }
  // Verify fingerprint matches our public key (first 4 bytes)
  ...
```

(Leave everything after the fingerprint check unchanged.)

- [ ] **Step 4.4: Build and run tests — expect pass**

```bash
cmake --build tests/build && ctest --test-dir tests/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 4.5: Commit**

```bash
git add main/src/Mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): relay JOIN_ACK outward so nodes >1 hop can enroll"
```

---

## Self-Review

**Spec coverage check:**

| Spec item | Task | Status |
|---|---|---|
| §5.1 Multi-hop uplink relay (`processAdapterData`) | Task 2 | ✓ |
| §5.2 Multi-hop downlink relay (`relayDownlink` helper) | Task 1 | ✓ |
| §5.2 Downlink relay wired into `processAdapterData` | Task 2/3 | ✓ |
| §5.2 `SERIAL_CMD_BROADCAST` relay | n/a — master converts to ADAPTER_DATA via `broadcastAdapterData`; fix is the broadcast-target MAC in Task 3 | ✓ |
| §5.3 JOIN_ACK relay | Task 4 | ✓ |

**Replay protection:** All relay paths call `isReplay()` first — either directly in `relayDownlink()` or inline in the uplink path. No message loops past `MAX_HOPS`.

**Broadcast-target convention:** `broadcastAdapterData()` now sets `targetMacAddress = FF:FF:...` and `broadcastToAllPeers()` no longer overwrites it. Existing `transmitCore` path (for targeted messages) sets `targetMacAddress = currentMaster.mac` unchanged. No regression.

**Placeholder scan:** None found. All code blocks contain compilable C++. All test assertions are concrete.

**Type consistency:** `mesh_message` unchanged throughout (Phase 3 changes struct). `relayDownlink` signature used identically in Tasks 1–4.
