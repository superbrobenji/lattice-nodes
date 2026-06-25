// Mock esp_err.h — shadows the ESP32 SDK header
#pragma once
#include <cstdint>

// esp_err_t and ESP_OK/ESP_FAIL may already be defined by esp_now_mock.h
// Guard against redefinition
#ifndef ESP_OK
#define ESP_OK   0
#define ESP_FAIL -1
typedef int esp_err_t;
#endif

inline const char* esp_err_to_name(esp_err_t err) {
  if (err == ESP_OK) return "ESP_OK";
  if (err == ESP_FAIL) return "ESP_FAIL";
  return "UNKNOWN_ERROR";
}
