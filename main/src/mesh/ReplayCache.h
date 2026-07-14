#pragma once
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

struct ReplayCache {
  static constexpr size_t CACHE_SIZE = 16;

  struct Entry {
    uint8_t mac[6];
    uint32_t epoch;
    uint16_t seq;
  };

  Entry cache[CACHE_SIZE]{};
  size_t idx{0};
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    idx = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
    memset(cache, 0, sizeof(cache));
  }

  uint16_t nextSeq() { return ++txSeqNum; }

  inline bool isReplay(const mesh_message& msg) {
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
      if (memcmp(cache[i].mac, msg.origin_mac_address, 6) == 0 &&
          cache[i].epoch == msg.epoch_num && cache[i].seq == msg.seq_num) {
        return true;
      }
    }
    memcpy(cache[idx].mac, msg.origin_mac_address, 6);
    cache[idx].epoch = msg.epoch_num;
    cache[idx].seq = msg.seq_num;
    idx = (idx + 1) % CACHE_SIZE;
    return false;
  }
};

} // namespace mesh
} // namespace lattice
