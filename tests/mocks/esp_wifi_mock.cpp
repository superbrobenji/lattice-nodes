#include "esp_wifi_mock.h"
#include <cstring>

int lastTxPowerSet = -1;
int lastPsModeSet = -1;
uint8_t mockDeviceMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
uint8_t mockWifiChannel = 1;
bool mockWifiStarted = false;

void resetWifiMock() {
  lastTxPowerSet = -1;
  lastPsModeSet = -1;
  mockWifiChannel = 1;
  mockWifiStarted = false;
}

int esp_wifi_set_max_tx_power(int8_t p) {
  lastTxPowerSet = p;
  return 0;
}
int esp_wifi_set_ps(wifi_ps_type_t type) {
  lastPsModeSet = type;
  return 0;
}
int esp_wifi_get_mac(int, uint8_t* mac) {
  memcpy(mac, mockDeviceMac, 6);
  return 0;
}
int esp_wifi_set_channel(uint8_t primary, int) {
  mockWifiChannel = primary;
  return 0;
}
int esp_wifi_get_channel(uint8_t* primary, int* second) {
  if (primary)
    *primary = mockWifiChannel;
  if (second)
    *second = 0;
  return 0;
}

// Phase I Task 3 (BB + ZZ): raw esp_wifi_init/start bring-up mocks — always
// succeed; host tests don't exercise the failure path of these calls.
esp_err_t esp_wifi_init(const wifi_init_config_t*) {
  return ESP_OK;
}
esp_err_t esp_wifi_set_storage(wifi_storage_t) {
  return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int) {
  return ESP_OK;
}
esp_err_t esp_wifi_start() {
  mockWifiStarted = true;
  return ESP_OK;
}
