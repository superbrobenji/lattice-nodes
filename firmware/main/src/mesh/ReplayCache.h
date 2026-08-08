#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"
#include "src/network/MacEq.h"
#include "src/network/mac_table.h"
#include "src/persistence/EepromManager.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"

namespace lattice {
namespace mesh {

struct ReplayCache {
  // Phase G audit item D: fields reordered (largest-alignment first) to save 4B
  // of padding per entry vs the original {mac[6], epoch, seq, lastSeenMs, used}
  // layout — mac[6]+epoch straddled a 4-byte boundary, forcing the compiler to
  // pad after mac. Field semantics are unchanged.
  struct Entry {
    uint32_t epoch;
    // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
    // millis() -> esp_timer_get_time()/1000ULL swap.
    uint64_t lastSeenMs;
    uint16_t seq;
    uint8_t mac[6];
    bool used;
  };

  Entry cache[config::LATTICE_REPLAY_MAX_ORIGINS]{};

  void init() { memset(cache, 0, sizeof(cache)); }

  // Return true if msg is a replay (drop it). Per-origin high-water:
  // accept iff strictly newer (epoch, seq) than the stored tuple.
  // First frame per origin allocates a slot (empty first, else LRU-evict by
  // lastSeenMs). LRU eviction of an active origin lets an attacker who first
  // floods > LATTICE_REPLAY_MAX_ORIGINS distinct origins re-deliver a genuine
  // older frame — AEAD still authenticates content, so worst-case is genuine
  // old delivery, not forgery. Size the knob to expected origins × 1.5.
  inline bool isReplay(const mesh_message& msg, uint64_t nowMs) {
    // 1. Find slot for this origin. Thinned via lattice::mac_table::find
    // (Phase H2 audit item Y).
    size_t found =
        lattice::mac_table::find(cache, config::LATTICE_REPLAY_MAX_ORIGINS, sizeof(Entry),
                                 offsetof(Entry, mac), msg.origin_mac_address);
    if (found != SIZE_MAX && cache[found].used) {
      bool newer = (msg.epoch_num > cache[found].epoch) ||
                   (msg.epoch_num == cache[found].epoch && msg.seq_num > cache[found].seq);
      if (!newer)
        return true;
      cache[found].epoch = msg.epoch_num;
      cache[found].seq = msg.seq_num;
      cache[found].lastSeenMs = nowMs;
      return false;
    }
    // 2. No slot — allocate: first !used, else LRU-evict (smallest lastSeenMs,
    // via lattice::mac_table::evict_oldest_by_ts).
    size_t slot = 0;
    bool foundEmpty = false;
    for (size_t i = 0; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
      if (!cache[i].used) {
        slot = i;
        foundEmpty = true;
        break;
      }
    }
    if (!foundEmpty) {
      slot = lattice::mac_table::evict_oldest_by_ts(cache, config::LATTICE_REPLAY_MAX_ORIGINS,
                                                    sizeof(Entry), offsetof(Entry, lastSeenMs));
    }
    memcpy(cache[slot].mac, msg.origin_mac_address, 6);
    cache[slot].epoch = msg.epoch_num;
    cache[slot].seq = msg.seq_num;
    cache[slot].lastSeenMs = nowMs;
    cache[slot].used = true;
    return false;
  }
};

// This node's own outbound sequence + relay-dedup bookkeeping — split out of
// ReplayCache (finding 15), which is scoped to incoming-message replay
// detection only. Round 2 Task 8: grew the seal-time AEAD nonce-reuse guard
// (nextSeqGuarded/checkEpochRollback, moved verbatim from Mesh) — the two
// sealed-epoch fields those methods own are private with the methods as their
// only mutators, unlike the 4 plain-public fields above them (still a small
// owned-state aggregate, not a class needing full access control, but state
// with a real invariant — never let seq/epoch go backwards — gets a real
// guard instead of bare field access).
struct OutboundSequenceState {
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
  }

  uint16_t nextSeq() { return ++txSeqNum; }
  void bumpEpoch(uint32_t newEpoch) { bootEpoch = newEpoch; }

  void markRelayed(uint32_t epoch, uint16_t seq) {
    lastRelayedEpoch = epoch;
    lastRelayedSeqNum = seq;
  }

  bool wasRelayedBefore(uint32_t epoch, uint16_t seq) const {
    bool isNewer =
        (epoch > lastRelayedEpoch) || (epoch == lastRelayedEpoch && seq > lastRelayedSeqNum);
    return !isNewer;
  }

  // Single choke point for drawing a tx sequence number. ALL sites that need
  // a fresh (epoch, seq) pair for a message this node originates MUST go
  // through this — it is the only place that guards against the 0xFFFF -> 0
  // wrap (spec §2): a reused (epoch, seq) pair after a silent wrap would
  // reuse an AEAD nonce. On wrap, bumps + persists the boot epoch before
  // redrawing so the new sequence starts under a fresh epoch. Moved verbatim
  // from Mesh::nextSeqGuarded (Round 2 Task 8).
  uint16_t nextSeqGuarded() {
    uint16_t seq = nextSeq();
    if (seq == 0) {
      uint32_t epoch = bootEpoch + 1;
      lattice::eeprom::saveBootEpoch(epoch);
      bumpEpoch(epoch);
      seq = nextSeq();
    }
    return seq;
  }

  // Seal-time AEAD nonce-reuse guard (Phase A, complements nextSeqGuarded's
  // wrap handling above): tracks the (epoch, seq) of the last frame this node
  // sealed. Call immediately before every sealPayload() call-site; halts the
  // node via lattice::err::fail(CRYPTO, MESH, 1) if the new (epoch, seq) does
  // not strictly advance, since that would reuse an AEAD nonce prefix under
  // the same key. UINT32_MAX in _lastSealedEpoch is the sentinel for "no seal
  // observed yet" (first call always passes). Moved verbatim from
  // Mesh::_checkEpochRollback (Round 2 Task 8).
  void checkEpochRollback(uint32_t epoch, uint16_t seq) {
    if (_lastSealedEpoch == UINT32_MAX) {
      _lastSealedEpoch = epoch;
      _lastSealedSeq = seq;
      return;
    }
    if (epoch > _lastSealedEpoch) {
      _lastSealedEpoch = epoch;
      _lastSealedSeq = seq;
      return;
    }
    if (epoch == _lastSealedEpoch && seq > _lastSealedSeq) {
      _lastSealedSeq = seq;
      return;
    }
    lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO, lattice::core::ModuleDigit::MESH, 1,
                       "AEAD epoch rollback — refusing seal");
  }

private:
  uint32_t _lastSealedEpoch = UINT32_MAX;
  uint16_t _lastSealedSeq = 0;
};

} // namespace mesh
} // namespace lattice
