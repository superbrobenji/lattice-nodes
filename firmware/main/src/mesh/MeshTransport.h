#ifndef MESH_TRANSPORT_H
#define MESH_TRANSPORT_H

#include <esp_now.h>
#include <esp_attr.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <cstdint>
#include <cstring>
#include "../../project_config.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

using ::mesh_message;

// Phase B Task 4 (finding 1 job 1; finding 19): owns ESP-NOW radio setup, the
// RX ring buffer + trampoline + drain, and outbound send primitives —
// extracted out of Mesh, which now holds one as a member and delegates all
// radio I/O to it. Message *dispatch* (what a received frame means) stays on
// Mesh: drain() takes a plain function-pointer handler and invokes it once
// per dequeued entry instead of interpreting message_type itself, since that
// dispatch reaches into Mesh-owned collaborators (Enrollment, etc.).
//
// See docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md, Task 4.
class MeshTransport {
public:
  // Dispatch callback invoked once per dequeued message by drain(). A plain
  // function pointer (not std::function) — mirrors Enrollment.h's
  // RegisterPeerFn/EnrollmentRelayFn pattern: the only production binding
  // (Mesh::drain()) goes through a static instance-trampoline
  // (Mesh::handleReceivedMessageTrampoline), not a capturing lambda.
  using MessageHandler = void (*)(const uint8_t* srcMac, const mesh_message& msg);

  MeshTransport();

  // Wi-Fi bring-up (moved verbatim from the old Mesh::setupWiFi, minus its
  // readMacAddress()/peers.setDeviceMac() tail — MAC address ownership stays
  // on Mesh; see Mesh::setupRadio() in Mesh.cpp).
  bool setup();

  // ESP-NOW init: sets the PMK from meshKey, registers the broadcast peer,
  // registers every already-known peer from `peers` (Task 1's const
  // iteration API), and installs the send/recv callbacks.
  bool setupEspNow(const uint8_t* meshKey, const PeerRegistry& peers);

  // Unicast send. deviceMac is needed to preserve the original
  // Mesh::sendMessage's "never send to self" guard without MeshTransport
  // holding a stored back-reference to Mesh's own MAC.
  void sendMessage(const uint8_t* target, const mesh_message& msg, const uint8_t* deviceMac);

  // Unicast msg to every registered peer except deviceMac (self).
  void broadcastToAllPeers(const mesh_message& msg, const PeerRegistry& peers,
                           const uint8_t* deviceMac);

  // Single choke point for esp_now_send(BROADCAST_MAC, ...) (post-Phase-G
  // audit item U) — see the original Mesh::sendBroadcast doc comment.
  // Static: no instance state needed, so Enrollment::sendRequest() — which
  // holds no Mesh*/MeshTransport* — can call it directly.
  static bool sendBroadcast(const mesh_message& msg);

  // Register an ESP-NOW peer WITHOUT link-layer encryption. Moved from
  // MeshCrypto.h (finding 19 — this is peering, not crypto). Static for the
  // same reason as sendBroadcast: Enrollment::enrollPeer() calls it directly
  // without holding a Mesh*/MeshTransport*.
  static void registerPeerWithEspNow(const uint8_t* mac);

  // Drain the RX ring buffer until empty, calling handler(srcMac, msg) once
  // per dequeued entry. Replaces the old Mesh::drainRecvQueue, which
  // dispatched inline — that dispatch (proto-version check, replay check,
  // peers.updateLastSeen, message-type switch into Mesh-owned handlers) now
  // lives in Mesh::handleReceivedMessage, reached via the handler passed
  // here.
  void drain(MessageHandler handler);

  // Registers the dedicated mesh task's handle so onDataRecvCallback's ISR
  // trampoline can wake it via vTaskNotifyGiveFromISR after enqueueing into
  // recvQueue. See the original Mesh::setDrainNotifyHandle doc comment.
  void setDrainNotifyHandle(TaskHandle_t handle) { drainNotifyHandle_ = handle; }

#if SIMULATE_MODE
  // Inject a message directly into the receive queue (bypasses radio — for dev/test only)
  void injectReceivedMessage(const uint8_t* srcMac, const mesh_message& msg) {
    RecvQueueEntry entry;
    memcpy(entry.srcMac, srcMac, 6);
    entry.msg = msg;
    // Non-blocking send (0 ticks); silently dropped if full, matching the
    // old array-based "Queue full — drop" behavior.
    xRingbufferSend(recvQueue, &entry, sizeof(entry), 0);
  }
#endif

#ifdef UNIT_TEST
  // In unit test builds, all members are public so test bodies (which live in
  // compiler-generated subclasses of the fixture and therefore cannot inherit
  // C++ friend access) can access private state directly. Mirrors Mesh.h's
  // own UNIT_TEST toggle.
public:
#else
private:
#endif
  static MeshTransport* instance;

  // --- ESP-NOW receive ring buffer (lock-free SPSC) ---
  // (Phase G §4) Trimmed 8 -> 4: observed fan-in during Phase A + B testing never
  // exceeded 3 concurrent frames; 4-deep gives a 1-slot margin. Raise back if real
  // deployments show queue-full drops.
  static constexpr size_t RECV_QUEUE_SIZE = 4;

  // Phase G §7: CompactMessage (src/mesh/CompactMessage.h) was evaluated for
  // recvQueue's element type but is NOT used here — see task-3-report.md.
  // Every message type that flows through this queue (ADAPTER_DATA downlink
  // relay, ROUTE_REPORT verify/relay, JOIN_ACK, ENROLLMENT relay) needs at
  // least one wire-only field CompactMessage cannot carry without matching
  // the wire message's own size (route_path/auth_tag/auth_path/
  // enrollment_public_key are load-bearing everywhere; secondary_master_mac/
  // secondary_public_key turned out to be load-bearing too — confirmed by
  // DualMasterTest.UplinkReachesSecondaryMasterAfterFailover and
  // ConfigSetFromSecondaryMasterIsHonoredAfterFailover failing when they were
  // dropped). RECV_QUEUE_SIZE above (8 -> 4) is this bundle's actual queue
  // RAM lever.
  struct RecvQueueEntry {
    uint8_t srcMac[6];
    mesh_message msg;
  };

  // Phase I Task 8 (item OO): static FreeRTOS ring buffer replaces the
  // hand-rolled head/tail/count SPSC array above. RINGBUF_TYPE_NOSPLIT keeps
  // each xRingbufferReceive() call returning a contiguous pointer to one
  // whole RecvQueueEntry (never split across the wrap boundary), matching
  // the old array's per-slot semantics. Storage is a static member array —
  // xRingbufferCreateStatic() places the ring entirely inside it, so this
  // stays heap-free like the array it replaces. The +128 pads for the ring's
  // internal per-item header overhead (a few bytes/item); real usage is
  // RECV_QUEUE_SIZE * (sizeof(RecvQueueEntry) + ~8).
  RingbufHandle_t recvQueue = nullptr;
  StaticRingbuffer_t _recvQueueStruct;
  uint8_t _recvQueueStorage[RECV_QUEUE_SIZE * sizeof(RecvQueueEntry) + 128];

  // Phase I Task 9 (item EE): handle of the dedicated mesh-drain task (owned by
  // main.cpp — see mesh_task_fn / setDrainNotifyHandle()). The RX-ISR
  // trampoline (onDataRecvCallback) notifies this task via
  // vTaskNotifyGiveFromISR immediately after enqueueing into recvQueue, so the
  // task wakes to drain instead of a polling loop discovering the item on its
  // next tick. Null (never set) is a valid state — the host unit-test /
  // SimNode harness has no real FreeRTOS task and calls drain() directly, and
  // onDataRecvCallback null-checks before notifying.
  TaskHandle_t drainNotifyHandle_ = nullptr;

  static void onDataSentCallback(const wifi_tx_info_t* mac_addr, esp_now_send_status_t status);
  void IRAM_ATTR onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData,
                                    int len);
  static void IRAM_ATTR dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data,
                                           int len);
};

} // namespace mesh
} // namespace lattice

#endif // MESH_TRANSPORT_H
