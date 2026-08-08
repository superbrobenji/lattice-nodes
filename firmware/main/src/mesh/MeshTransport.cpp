#include "MeshTransport.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/network/MacEq.h"
#include "broadcast_mac.h"
#include <cstdio>
#include <cstring>

namespace lattice {
namespace mesh {

using namespace lattice::utils;

MeshTransport* MeshTransport::instance = nullptr;

MeshTransport::MeshTransport() {
  instance = this;
  // Phase I Task 8 (item OO): static ring buffer over _recvQueueStorage —
  // heap-free, matching the array it replaces. Storage is a MeshTransport
  // member so its lifetime matches the instance; safe to create
  // unconditionally here (doesn't depend on WiFi/ESP-NOW being up yet).
  recvQueue = xRingbufferCreateStatic(sizeof(_recvQueueStorage), RINGBUF_TYPE_NOSPLIT,
                                      _recvQueueStorage, &_recvQueueStruct);
}

bool MeshTransport::setup() {
  // Phase I Task 3 (BB + ZZ): raw ESP-IDF WiFi bring-up, replacing the
  // arduino-esp32 WiFi.mode(WIFI_STA) wrapper. ESP-NOW is our only WiFi use
  // (no AP association, no persisted creds) so STA mode + WIFI_STORAGE_RAM
  // (making CONFIG_ESP_WIFI_NVS_ENABLED=n fully effective) is sufficient.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  lattice::err::checkEsp(esp_wifi_set_channel(lattice::config::WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE),
                         lattice::utils::ErrorType::HARDWARE_FAILURE, "Failed to set WiFi channel");
  return true;
}

bool MeshTransport::setupEspNow(const uint8_t* meshKey, const PeerRegistry& peers) {
  esp_err_t res = esp_now_init();
  if (res != ESP_OK) {
    // Phase I Task 7 (TT): String() temporary eliminated.
    char errBuf[80];
    snprintf(errBuf, sizeof(errBuf), "MESH: esp_now_init failed: %s", esp_err_to_name(res));
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 3,
                       errBuf);
    return false;
  }
  lattice::err::checkEsp(esp_now_set_pmk(meshKey), lattice::utils::ErrorType::HARDWARE_FAILURE,
                         "Failed to set ESP-NOW PMK");

  // Register the broadcast MAC so esp_now_send(BROADCAST_MAC, ...) reaches all
  // nodes — including unregistered ones. esp_now_send(nullptr, ...) only delivers
  // to already-registered peers; using the explicit FF:FF:… MAC is required for a
  // true 802.11 broadcast frame.
  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
    esp_now_peer_info_t broadcast = {};
    memset(broadcast.peer_addr, 0xFF, 6);
    broadcast.channel = 0;
    broadcast.encrypt = false;
    esp_now_add_peer(&broadcast);
  }

  for (const auto& p : peers) {
    registerPeerWithEspNow(p.mac);
  }
  esp_now_register_send_cb(onDataSentCallback);
  esp_now_register_recv_cb(MeshTransport::dataRecvTrampoline);
  LATTICE_LOGLN("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void MeshTransport::onDataSentCallback(const wifi_tx_info_t* mac_addr,
                                       esp_now_send_status_t status) {
  // Inlined into the LATTICE_LOGF call (rather than a local `statusStr`) so that
  // under LOG_NONE, where the whole call folds to ((void)0), there's no
  // now-unused local left behind to warn about.
  LATTICE_LOGF("MESH", LogLevel::LOG_DEBUG, "Last Packet Send Status: %s",
               (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail");
}

void IRAM_ATTR MeshTransport::onDataRecvCallback(const esp_now_recv_info* info,
                                                 const uint8_t* incomingData, int len) {
  if (!instance || !info || !incomingData)
    return;
  if (static_cast<size_t>(len) < sizeof(mesh_message))
    return;

  RecvQueueEntry entry;
  memcpy(entry.srcMac, info->src_addr, 6);
  memcpy(&entry.msg, incomingData, sizeof(mesh_message));

  BaseType_t woken = pdFALSE;
  // Queue full — xRingbufferSendFromISR returns pdFALSE and the packet is
  // silently dropped, matching the old array's "Queue full — drop" behavior
  // (no logging here: this runs in WiFi task/ISR context, and Serial writes
  // are not safe from that context — see Mesh::loop()'s comment on
  // drainPendingRelay).
  xRingbufferSendFromISR(instance->recvQueue, &entry, sizeof(entry), &woken);

  // Phase I Task 9 (item EE): wake the dedicated mesh-drain task instead of
  // leaving drain() to be discovered by a polling loop() iteration — this is
  // what lets loop() (and therefore the FreeRTOS idle task) go idle between
  // real work instead of busy-checking recvQueue every tick, which is a
  // prerequisite for tickless idle / light sleep to pay off. Null handle
  // (host/SimNode builds, or a real boot before setDrainNotifyHandle() has
  // run yet) is a no-op here — the item just waits in recvQueue for the next
  // explicit drain() call.
  BaseType_t woken2 = pdFALSE;
  if (instance->drainNotifyHandle_ != nullptr) {
    vTaskNotifyGiveFromISR(instance->drainNotifyHandle_, &woken2);
  }
  if (woken || woken2)
    portYIELD_FROM_ISR();
}

void IRAM_ATTR MeshTransport::dataRecvTrampoline(const esp_now_recv_info* mac_addr,
                                                 const uint8_t* data, int len) {
  if (!instance)
    return;
  instance->onDataRecvCallback(mac_addr, data, len);
}

void MeshTransport::drain(MessageHandler handler) {
  size_t itemSize = 0;
  RecvQueueEntry* entryPtr;
  while ((entryPtr = static_cast<RecvQueueEntry*>(xRingbufferReceive(recvQueue, &itemSize, 0))) !=
         nullptr) {
    if (itemSize != sizeof(RecvQueueEntry)) {
      // Should never happen (NOSPLIT items are always sent whole) — guard
      // against a corrupt/short item rather than reading past it.
      vRingbufferReturnItem(recvQueue, entryPtr);
      continue;
    }
    RecvQueueEntry entry = *entryPtr;
    vRingbufferReturnItem(recvQueue, entryPtr);

    if (handler)
      handler(entry.srcMac, entry.msg);
  }
}

void MeshTransport::sendMessage(const uint8_t* target, const mesh_message& msg,
                                const uint8_t* deviceMac) {
  if (lattice::mac::eq(target, deviceMac)) {
    LATTICE_LOGLN("MESH", "Not sending to self. Skipped.", LogLevel::LOG_DEBUG);
    return;
  }
  esp_err_t result = esp_now_send(target, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (result == ESP_OK) {
    LATTICE_LOGLN("MESH", "Message sent to peer", LogLevel::LOG_DEBUG);
  } else {
    // Phase I Task 7 (TT): String() temporary eliminated.
    char errBuf[80];
    snprintf(errBuf, sizeof(errBuf), "MESH: Error sending message: %s", esp_err_to_name(result));
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 5,
                       errBuf);
  }
}

void MeshTransport::broadcastToAllPeers(const mesh_message& msg, const PeerRegistry& peers,
                                        const uint8_t* deviceMac) {
  if (peers.count() == 0) {
    LATTICE_LOGLN("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (const auto& p : peers) {
    if (lattice::mac::eq(p.mac, deviceMac))
      continue; // Skip self
    sendMessage(p.mac, msg, deviceMac);
  }
}

bool MeshTransport::sendBroadcast(const mesh_message& msg) {
  esp_err_t err = esp_now_send(BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (err != ESP_OK) {
    LATTICE_LOGF("MESH", LogLevel::LOG_WARN, "Broadcast send failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// Register an ESP-NOW peer WITHOUT link-layer encryption (spec §2, proto v3):
// payload confidentiality/integrity is end-to-end (E2ECrypto.h), and unencrypted
// slots raise the ESP-NOW peer cap from ~6 to 20. The shared PMK stays set.
// Moved from MeshCrypto.h (finding 19 — this is peering, not crypto).
void MeshTransport::registerPeerWithEspNow(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac))
    return;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = false;
  lattice::err::checkEsp(esp_now_add_peer(&info), lattice::utils::ErrorType::COMMUNICATION_FAIL,
                         "registerPeerWithEspNow: add_peer failed");
}

} // namespace mesh
} // namespace lattice
