#pragma once
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"
#include "src/network/MacEq.h"

namespace lattice {
namespace mesh {

struct ReplayCache {
  // Phase G audit item D: fields reordered (largest-alignment first) to save 4B
  // of padding per entry vs the original {mac[6], epoch, seq, lastSeenMs, used}
  // layout — mac[6]+epoch straddled a 4-byte boundary, forcing the compiler to
  // pad after mac. Field semantics are unchanged.
  struct Entry {
    uint32_t epoch;
    uint32_t lastSeenMs;
    uint16_t seq;
    uint8_t mac[6];
    bool used;
  };

  Entry cache[config::LATTICE_REPLAY_MAX_ORIGINS]{};
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
    memset(cache, 0, sizeof(cache));
  }

  uint16_t nextSeq() { return ++txSeqNum; }

  // Return true if msg is a replay (drop it). Per-origin high-water:
  // accept iff strictly newer (epoch, seq) than the stored tuple.
  // First frame per origin allocates a slot (empty first, else LRU-evict by
  // lastSeenMs). LRU eviction of an active origin lets an attacker who first
  // floods > LATTICE_REPLAY_MAX_ORIGINS distinct origins re-deliver a genuine
  // older frame — AEAD still authenticates content, so worst-case is genuine
  // old delivery, not forgery. Size the knob to expected origins × 1.5.
  inline bool isReplay(const mesh_message& msg, uint32_t nowMs) {
    // 1. Find slot for this origin.
    for (size_t i = 0; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
      if (cache[i].used && lattice::mac::eq(cache[i].mac, msg.origin_mac_address)) {
        bool newer = (msg.epoch_num > cache[i].epoch) ||
                     (msg.epoch_num == cache[i].epoch && msg.seq_num > cache[i].seq);
        if (!newer)
          return true;
        cache[i].epoch = msg.epoch_num;
        cache[i].seq = msg.seq_num;
        cache[i].lastSeenMs = nowMs;
        return false;
      }
    }
    // 2. No slot — allocate: first !used, else LRU-evict (smallest lastSeenMs).
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
      slot = 0;
      for (size_t i = 1; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
        if (cache[i].lastSeenMs < cache[slot].lastSeenMs)
          slot = i;
      }
    }
    memcpy(cache[slot].mac, msg.origin_mac_address, 6);
    cache[slot].epoch = msg.epoch_num;
    cache[slot].seq = msg.seq_num;
    cache[slot].lastSeenMs = nowMs;
    cache[slot].used = true;
    return false;
  }
};

} // namespace mesh
} // namespace lattice
