#pragma once
// Mock nvs_flash.h — shadows the ESP-IDF nvs_flash component's top-level
// header. Only nvs_flash_init()/nvs_flash_erase() live here (main.cpp's boot
// sequence, Phase I Task 4); the handle-based get/set API is in nvs.h.
// Not exercised by host unit/e2e tests today (main.cpp isn't part of the host
// build), but kept in step with the real header split so EepromManager.h's
// #include set matches the ESP-IDF target 1:1.
#include "esp_err.h"
#include "nvs.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
