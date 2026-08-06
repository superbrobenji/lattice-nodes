#pragma once
#include <cstdint>
#include "esp_err.h"

#define WIFI_PS_NONE 0
#define WIFI_PS_MIN_MODEM 1
#define WIFI_PS_MAX_MODEM 2
typedef int wifi_ps_type_t;

// WIFI_IF_STA is used by ProtobufCodec and Serial_Adapter
typedef int wifi_interface_t;
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1

#define WIFI_SECOND_CHAN_NONE 0

// WiFi mode + storage — may already be defined by WiFi.h (Arduino-compat
// mock, still pulled in transitively by esp_now_mock.h). Guard against
// redefinition either way round.
#ifndef WIFI_STA
#define WIFI_STA 1
#define WIFI_MODE_STA WIFI_STA
#endif

// Phase I Task 3 (BB + ZZ): raw esp_wifi_init/start bring-up mocks.
typedef int wifi_storage_t;
#define WIFI_STORAGE_FLASH 0
#define WIFI_STORAGE_RAM 1

// Real wifi_init_config_t is a large struct populated by WIFI_INIT_CONFIG_DEFAULT();
// host tests never inspect its fields, so an empty placeholder is sufficient.
struct wifi_init_config_t {
  int placeholder;
};
#define WIFI_INIT_CONFIG_DEFAULT() (wifi_init_config_t{})

extern int lastTxPowerSet;
extern int lastPsModeSet;
extern uint8_t mockDeviceMac[6];
extern uint8_t mockWifiChannel;
extern bool mockWifiStarted;

void resetWifiMock();

int esp_wifi_set_max_tx_power(int8_t power);
int esp_wifi_set_ps(wifi_ps_type_t type);
int esp_wifi_get_mac(int ifx, uint8_t* mac);
int esp_wifi_set_channel(uint8_t primary, int second);
int esp_wifi_get_channel(uint8_t* primary, int* second);
esp_err_t esp_wifi_init(const wifi_init_config_t* config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start();
