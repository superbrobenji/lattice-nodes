// Mock esp_event.h — shadows the ESP32 SDK header
//
// Phase I Task 3 (BB + ZZ): Mesh::setupWiFi() now calls
// esp_event_loop_create_default() directly (replacing arduino-esp32's
// WiFi.mode() wrapper, which did this internally). Host tests never dispatch
// real events through it, so the mock is a no-op that always succeeds.
#pragma once
#include "esp_err.h"

inline esp_err_t esp_event_loop_create_default() {
  return ESP_OK;
}
