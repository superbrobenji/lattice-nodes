// Mock freertos/ringbuf.h — shadows the ESP-IDF SDK header (Phase I Task 8,
// item OO). Backs Mesh::recvQueue and Enrollment::_pendingRelayQueue with a
// std::deque-based in-memory buffer standing in for the real static,
// heap-free ESP-IDF ring buffer (xRingbufferCreateStatic over
// caller-provided storage). Host tests aren't RAM-constrained the way the
// firmware target is, so unlike the ESP32 side this mock is free to use the
// heap — it only needs to reproduce the state-machine behavior real callers
// depend on: FIFO ordering, a real capacity ceiling that can report "full",
// and give-back-ownership-on-receive semantics via vRingbufferReturnItem().
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include "freertos/FreeRTOS.h"

typedef enum {
  RINGBUF_TYPE_NOSPLIT = 0,
  RINGBUF_TYPE_ALLOWSPLIT,
  RINGBUF_TYPE_BYTEBUF,
} RingbufferType_t;

// Real ESP-IDF's StaticRingbuffer_t is an opaque, fixed-size blob the caller
// provides storage for; the mock keeps no live state inside it (see
// RingbufMockState below), so this only needs to exist to satisfy call sites
// that take its address.
typedef struct {
  uint8_t _unused[4];
} StaticRingbuffer_t;

// Internal mock state a RingbufHandle_t points to. Mirrors real NOSPLIT
// accounting closely enough (8-byte per-item header overhead, matching the
// real implementation) to reject sends once the configured byte budget is
// exhausted — so the "queue full — drop" paths in Mesh::onDataRecvCallback /
// Enrollment::enqueuePendingRelay get exercised under test the same way they
// are on-device.
struct RingbufMockState {
  size_t capacityBytes = 0;
  size_t usedBytes = 0;
  std::deque<std::vector<uint8_t>> items;
};

typedef RingbufMockState* RingbufHandle_t;

inline RingbufHandle_t xRingbufferCreateStatic(size_t xBufferSize, RingbufferType_t /*xBufferType*/,
                                               uint8_t* /*pucRingbufferStorage*/,
                                               StaticRingbuffer_t* /*pxStaticRingbuffer*/) {
  RingbufMockState* state = new RingbufMockState();
  state->capacityBytes = xBufferSize;
  return state;
}

inline BaseType_t xRingbufferSend(RingbufHandle_t xRingbuffer, const void* pvItem, size_t xItemSize,
                                  TickType_t /*xTicksToWait*/) {
  if (!xRingbuffer)
    return pdFALSE;
  if (xRingbuffer->usedBytes + xItemSize + 8 > xRingbuffer->capacityBytes)
    return pdFALSE; // full — same as the real NOSPLIT ring returning pdFALSE
  const uint8_t* bytes = static_cast<const uint8_t*>(pvItem);
  xRingbuffer->items.emplace_back(bytes, bytes + xItemSize);
  xRingbuffer->usedBytes += xItemSize + 8;
  return pdTRUE;
}

inline BaseType_t xRingbufferSendFromISR(RingbufHandle_t xRingbuffer, const void* pvItem,
                                         size_t xItemSize, BaseType_t* pxHigherPriorityTaskWoken) {
  if (pxHigherPriorityTaskWoken)
    *pxHigherPriorityTaskWoken = pdFALSE; // no real scheduler on host to wake
  return xRingbufferSend(xRingbuffer, pvItem, xItemSize, 0);
}

// Caller owns the returned pointer until it calls vRingbufferReturnItem() —
// mirrors the real API's "pointer valid until returned" contract. The mock
// heap-allocates a private copy per receive rather than pointing into ring
// storage directly; callers never retain the pointer past processing +
// return-item, so this is transparent to them.
inline void* xRingbufferReceive(RingbufHandle_t xRingbuffer, size_t* pxItemSize,
                                TickType_t /*xTicksToWait*/) {
  if (!xRingbuffer || xRingbuffer->items.empty()) {
    if (pxItemSize)
      *pxItemSize = 0;
    return nullptr;
  }
  std::vector<uint8_t>& front = xRingbuffer->items.front();
  uint8_t* copy = new uint8_t[front.size()];
  memcpy(copy, front.data(), front.size());
  if (pxItemSize)
    *pxItemSize = front.size();
  xRingbuffer->usedBytes -= front.size() + 8;
  xRingbuffer->items.pop_front();
  return copy;
}

inline void vRingbufferReturnItem(RingbufHandle_t /*xRingbuffer*/, void* pvItem) {
  delete[] static_cast<uint8_t*>(pvItem);
}
