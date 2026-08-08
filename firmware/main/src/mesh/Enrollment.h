#pragma once
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "PendingRelayQueue.h"

namespace lattice {
namespace mesh {

using EnrollmentRelayFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);
// Returns false if the peer could not be registered (e.g. registry full).
// Plain function pointer (post-Phase-G audit item H): the only production
// binding (Mesh::processJoinAck) goes through a static trampoline
// (Mesh::registerPeerWithKeyTrampoline) instead of a `[this]`-capturing
// lambda, since a capturing lambda cannot convert to a function pointer.
using RegisterPeerFn = bool (*)(const uint8_t* mac, const uint8_t* pubKey32);

class Enrollment {
  friend class Mesh;

public:
  Enrollment();
  void init(); // loads or generates keypair; loads enrolled flag + TOFU MACs from EEPROM

  bool isEnrolled() const;
  const uint8_t* getPublicKey() const { return devicePublicKey; }
  const uint8_t* getPrivateKey() const { return devicePrivateKey; }

  void sendRequest(const uint8_t* deviceMac, uint8_t protoVersion, uint32_t epochNum,
                   uint16_t seqNum);
  void processRequest(const mesh_message& msg);
  void processJoinAck(const mesh_message& msg, const uint8_t* deviceMac, RegisterPeerFn registerFn);
  void enrollPeer(const uint8_t* mac, const uint8_t* pubKey32, RegisterPeerFn registerFn,
                  bool dualMasterMode);

  void setRelayFn(EnrollmentRelayFn fn);
  void setPendingRelay(const uint8_t* mac, const uint8_t* pubKey);
  void drainPendingRelay();

  // Owns the memcpy + flag-set + EEPROM-persist triple for TOFU-learning the
  // (primary/secondary) master MAC — replaces 3 duplicated inline sites that
  // were in Mesh.cpp (2 later relocated to MasterBeacon.cpp, Phase B Task 5)
  // and 2 in this file's own processJoinAck() (finding 6).
  void learnMasterMac(const uint8_t* mac);
  void learnSecondaryMasterMac(const uint8_t* mac);

  // Read accessors for the 4 TOFU fields below (Phase B Task 5, finding 1 job
  // 3). Unlike `Mesh` (granted `friend class Mesh;` above for its own
  // permanent read needs), `MasterBeacon` is new code with no such
  // friendship — these give it a real API for the reads its moved
  // processMasterBeacon body needs, instead of growing the friend list.
  bool hasKnownMaster() const { return hasMasterMac; }
  const uint8_t* knownMaster() const { return knownMasterMac; }
  bool hasKnownSecondaryMaster() const { return hasMasterMacSecondary; }
  const uint8_t* knownSecondaryMaster() const { return knownMasterMacSecondary; }

#ifdef UNIT_TEST
public:
#else
private:
#endif
  uint8_t devicePrivateKey[32]{};
  uint8_t devicePublicKey[32]{};
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};

  // Cached mirror of the NVS "enrolled" flag (post-Phase-G audit item G).
  // isEnrolled() used to call EepromManager::loadEnrolledFlag() -> NVS read
  // on every call — main.cpp's loop() calls it 2-3x per iteration. Loaded
  // once from NVS in init(), flipped true the moment processJoinAck()
  // persists the flag; never cleared once set (matches loadEnrolledFlag()'s
  // one-way semantics — nothing in this codebase un-enrolls a node).
  bool _enrolled{false};

  // Bounded, heap-free FIFO of enrollment requests awaiting relay to the server.
  // Sized to RECV_QUEUE_SIZE (Phase G §4: 4): the master drains this queue once per
  // loop() AFTER draining its ESP-NOW receive ring, so the enrollment requests from
  // one drain pass accumulate here before being relayed in a batch. A single-slot
  // latch (the previous design) silently dropped all but the last when two nodes
  // enrolled concurrently — see Task 9b Bug #6. Extracted into its own
  // PendingRelayQueue type (finding 16) — see PendingRelayQueue.h for the
  // ring-buffer plumbing.
  PendingRelayQueue _relayQueue;

  EnrollmentRelayFn _enrollmentRelayFn{nullptr};
};

} // namespace mesh
} // namespace lattice
