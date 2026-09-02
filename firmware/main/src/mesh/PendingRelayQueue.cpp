#include "PendingRelayQueue.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace mesh {

using namespace lattice::utils;

PendingRelayQueue::PendingRelayQueue() {
  _queue = xRingbufferCreateStatic(sizeof(_storage), RINGBUF_TYPE_NOSPLIT, _storage, &_queueStruct);
}

void PendingRelayQueue::push(const uint8_t* mac, const uint8_t* pubKey) {
  Entry entry;
  memcpy(entry.mac, mac, 6);
  memcpy(entry.pubKey, pubKey, 32);
  if (xRingbufferSend(_queue, &entry, sizeof(entry), 0) != pdTRUE) {
    LATTICE_LOGLN("MESH", "Enrollment relay queue full — dropping request", LogLevel::LOG_WARN);
  }
}

void PendingRelayQueue::drainTo(PendingRelayDrainFn fn) {
  size_t itemSize = 0;
  Entry* entryPtr;
  while ((entryPtr = static_cast<Entry*>(xRingbufferReceive(_queue, &itemSize, 0))) != nullptr) {
    if (itemSize == sizeof(Entry) && fn) {
      fn(entryPtr->mac, entryPtr->pubKey);
    }
    vRingbufferReturnItem(_queue, entryPtr);
  }
}

} // namespace mesh
} // namespace lattice
