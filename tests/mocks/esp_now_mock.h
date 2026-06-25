#pragma once
#include <cstdint>
#include <vector>
#include <functional>

#define ESP_OK    0
#define ESP_FAIL -1
typedef int esp_err_t;

struct esp_now_peer_info_t {
  uint8_t peer_addr[6];
  uint8_t lmk[16];
  bool    encrypt;
  int     channel;
};

struct esp_now_recv_info {
  const uint8_t* src_addr;
};

// Captured outgoing sends — inspect in tests
struct EspNowSend {
  bool isBroadcast;
  uint8_t addr[6];
  std::vector<uint8_t> data;
};

extern std::vector<EspNowSend>          espNowSentPackets;
extern std::vector<esp_now_peer_info_t> espNowRegisteredPeers;
extern bool                             espNowSendFails;

// Reset between tests
void resetEspNowMock();

// Inject a received packet (triggers registered callback)
void simulateReceive(const uint8_t* srcMac, const uint8_t* data, int len);

// ESP-NOW API mocks
esp_err_t esp_now_init();
esp_err_t esp_now_deinit();
esp_err_t esp_now_register_recv_cb(void (*cb)(const esp_now_recv_info*, const uint8_t*, int));
esp_err_t esp_now_send(const uint8_t* peer_addr, const uint8_t* data, size_t len);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t* peer);
esp_err_t esp_now_del_peer(const uint8_t* peer_addr);
bool      esp_now_is_peer_exist(const uint8_t* peer_addr);
esp_err_t esp_now_set_pmk(const uint8_t* pmk);
