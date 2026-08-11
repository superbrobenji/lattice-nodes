// Mock esp_netif.h — shadows the ESP32 SDK header
//
// Phase I Task 3 (BB + ZZ): Mesh::setupWiFi() now calls esp_netif_init()
// directly (replacing arduino-esp32's WiFi.mode() wrapper, which did this
// internally). ESP-NOW doesn't need an actual netif instance — just the
// one-time lwIP/netif subsystem bring-up — so the host mock is a no-op that
// always succeeds.
#pragma once
#include "esp_err.h"

inline esp_err_t esp_netif_init() {
  return ESP_OK;
}
