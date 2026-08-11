#pragma once
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

namespace lattice {
namespace mesh {

using PendingRelayDrainFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);

// Bounded, heap-free FIFO of (mac, pubKey) pairs — extracted from Enrollment
// (finding 16), which used this shape for enrollment requests awaiting relay
// to the server. Not templated/generic (YAGNI — this codebase avoids
// per-instantiation template bloat elsewhere, e.g. network/mac_table.h).
class PendingRelayQueue {
public:
  struct Entry {
    uint8_t mac[6];
    uint8_t pubKey[32];
  };

  static constexpr size_t CAPACITY = 4; // matches the old PENDING_RELAY_QUEUE_SIZE

  PendingRelayQueue();

  // Drops (with LOG_WARN) if full. Call only from task context, never ISR —
  // matches the original call site (Mesh::drainRecvQueue, not the RX ISR).
  void push(const uint8_t* mac, const uint8_t* pubKey);

  // Drains every queued entry per call, invoking fn(mac, pubKey) for each.
  void drainTo(PendingRelayDrainFn fn);

// UNIT_TEST-guarded (same idiom as Enrollment.h): production code never reads
// queue occupancy directly (push/drainTo are the only intended API), but
// several pre-existing Enrollment-relay tests (test_mesh_logic.cpp) assert on
// in-flight queue depth via the mock ring buffer's `items` field, same as
// they did before this extraction.
#ifdef UNIT_TEST
public:
#else
private:
#endif
  RingbufHandle_t _queue = nullptr;
  StaticRingbuffer_t _queueStruct;
  uint8_t _storage[CAPACITY * sizeof(Entry) + 128];
};

} // namespace mesh
} // namespace lattice
