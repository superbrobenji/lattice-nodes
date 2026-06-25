#pragma once
#include <cstdint>

#define WIFI_PS_NONE      0
#define WIFI_PS_MIN_MODEM 1
#define WIFI_PS_MAX_MODEM 2
typedef int wifi_ps_type_t;

// WIFI_IF_STA is used by ProtobufCodec and Serial_Adapter
typedef int wifi_interface_t;
#define WIFI_IF_STA  0
#define WIFI_IF_AP   1

extern int  lastTxPowerSet;
extern int  lastPsModeSet;
extern uint8_t mockDeviceMac[6];

void resetWifiMock();

int esp_wifi_set_max_tx_power(int8_t power);
int esp_wifi_set_ps(wifi_ps_type_t type);
int esp_wifi_get_mac(int ifx, uint8_t* mac);
