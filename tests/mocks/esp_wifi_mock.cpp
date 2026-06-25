#include "esp_wifi_mock.h"
#include <cstring>

int     lastTxPowerSet = -1;
int     lastPsModeSet  = -1;
uint8_t mockDeviceMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

void resetWifiMock() {
  lastTxPowerSet = -1;
  lastPsModeSet  = -1;
}

int esp_wifi_set_max_tx_power(int8_t p)  { lastTxPowerSet = p; return 0; }
int esp_wifi_set_ps(wifi_ps_type_t type) { lastPsModeSet = type; return 0; }
int esp_wifi_get_mac(int, uint8_t* mac)  { memcpy(mac, mockDeviceMac, 6); return 0; }
