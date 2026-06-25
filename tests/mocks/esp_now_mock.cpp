#include "esp_now_mock.h"
#include <cstring>
#include <algorithm>

std::vector<EspNowSend>          espNowSentPackets;
std::vector<esp_now_peer_info_t> espNowRegisteredPeers;
bool                             espNowSendFails = false;

static void (*recvCallback)(const esp_now_recv_info*, const uint8_t*, int) = nullptr;

void resetEspNowMock() {
  espNowSentPackets.clear();
  espNowRegisteredPeers.clear();
  espNowSendFails = false;
  recvCallback = nullptr;
}

void simulateReceive(const uint8_t* srcMac, const uint8_t* data, int len) {
  if (!recvCallback) return;
  esp_now_recv_info info{srcMac};
  recvCallback(&info, data, len);
}

esp_err_t esp_now_init()   { return ESP_OK; }
esp_err_t esp_now_deinit() { return ESP_OK; }

esp_err_t esp_now_register_recv_cb(void (*cb)(const esp_now_recv_info*, const uint8_t*, int)) {
  recvCallback = cb;
  return ESP_OK;
}

esp_err_t esp_now_send(const uint8_t* peer_addr, const uint8_t* data, size_t len) {
  if (espNowSendFails) return ESP_FAIL;
  EspNowSend s{};
  s.isBroadcast = (peer_addr == nullptr);
  if (peer_addr) memcpy(s.addr, peer_addr, 6);
  s.data.assign(data, data + len);
  espNowSentPackets.push_back(std::move(s));
  return ESP_OK;
}

esp_err_t esp_now_add_peer(const esp_now_peer_info_t* peer) {
  espNowRegisteredPeers.push_back(*peer);
  return ESP_OK;
}

esp_err_t esp_now_del_peer(const uint8_t* addr) {
  espNowRegisteredPeers.erase(
    std::remove_if(espNowRegisteredPeers.begin(), espNowRegisteredPeers.end(),
      [addr](const auto& p) { return memcmp(p.peer_addr, addr, 6) == 0; }),
    espNowRegisteredPeers.end());
  return ESP_OK;
}

bool esp_now_is_peer_exist(const uint8_t* addr) {
  return std::any_of(espNowRegisteredPeers.begin(), espNowRegisteredPeers.end(),
    [addr](const auto& p) { return memcmp(p.peer_addr, addr, 6) == 0; });
}

esp_err_t esp_now_set_pmk(const uint8_t*) { return ESP_OK; }
