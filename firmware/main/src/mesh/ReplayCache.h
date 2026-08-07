#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"
#include "src/network/MacEq.h"
#include "src/network/mac_table.h"

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

  void init() {
    memset(cache, 0, sizeof(cache));
  }

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
// detection only. Plain public fields, same as ReplayCache's own cache[] is
// internally: this is a small owned-state aggregate on Mesh, not a class
// needing its own access control.
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
    bool isNewer = (epoch > lastRelayedEpoch) ||
                   (epoch == lastRelayedEpoch && seq > lastRelayedSeqNum);
    return !isNewer;
  }
};

} // namespace mesh
} // namespace lattice
