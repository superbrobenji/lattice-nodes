// Mock WiFi.h — shadows the Arduino ESP32 WiFi header
#pragma once
#include <cstdint>

// WiFi modes — may already be defined by esp_wifi_mock.h (Phase I Task 3
// raw-init mocks) depending on include order. Guard against redefinition
// either way round.
#ifndef WIFI_STA
#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_MODE_STA WIFI_STA
#define WIFI_MODE_AP WIFI_AP
#endif

// Types used by Mesh.h callbacks
typedef struct {
  uint8_t mac[6];
} wifi_tx_info_t;

typedef enum {
  ESP_NOW_SEND_SUCCESS = 0,
  ESP_NOW_SEND_FAIL = 1,
} esp_now_send_status_t;

class WiFiClass {
public:
  bool mode(int) { return true; }
  bool disconnect(bool = false) { return true; }
  uint8_t channel() { return 1; }
};

extern WiFiClass WiFi;
