#pragma once
// Mock nvs.h — shadows the ESP-IDF nvs_flash component's nvs.h for host tests.
//
// Backs EepromManager's nvs_open/nvs_get_*/nvs_set_*/nvs_commit/nvs_close/
// nvs_erase_* calls (Phase I Task 4: nvs_flash direct) with an in-memory
// "namespace/key" -> bytes map, keyed the same way tests/mocks/Preferences.cpp
// used to key its store. Semantics deliberately mirror the real ESP-IDF nvs
// component where it matters for this firmware's call patterns:
//   - nvs_get_blob(handle, key, NULL, &length) queries the stored size without
//     copying (used by NvsMock-backed hasKey()-style checks).
//   - nvs_get_blob with a too-small output buffer returns
//     ESP_ERR_NVS_INVALID_LENGTH and does NOT partially copy.
//   - nvs_get_blob with a buffer >= stored size copies only the actual stored
//     length and reports that length back via *length (matches real IDF; the
//     untouched remainder of a caller's larger buffer is left as-is, which is
//     why EepromManager::loadPeerList() prefills its buffer with 0xFF before
//     calling in).
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "esp_err.h"

// Numeric offsets mirror components/nvs_flash/include/nvs.h in the real
// ESP-IDF (v5.5.1) exactly — not load-bearing for the mock's own logic
// (which only compares symbols), but keeping them aligned avoids confusion
// when cross-referencing real IDF error logs against host test output.
#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_REMOVE_FAILED (ESP_ERR_NVS_BASE + 0x08)
#define ESP_ERR_NVS_KEY_TOO_LONG (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_PAGE_FULL (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_INVALID_STATE (ESP_ERR_NVS_BASE + 0x0b)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_VALUE_TOO_LONG (ESP_ERR_NVS_BASE + 0x0e)
#define ESP_ERR_NVS_PART_NOT_FOUND (ESP_ERR_NVS_BASE + 0x0f)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)
#endif

typedef uint32_t nvs_handle_t;

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value);

esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value);

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

// Test-only inspection/injection hooks (parallels the old Preferences mock's
// Preferences::_store / Preferences::_shortWriteKey). Store key format is
// "<namespace>/<key>", same convention Preferences.cpp used.
class NvsMock {
public:
  static std::map<std::string, std::vector<uint8_t>> _store;

  // When non-null, the next nvs_set_u8/nvs_set_u32/nvs_set_blob call whose key
  // matches returns ESP_FAIL (simulating a failed NVS write) instead of
  // writing. Caller resets to nullptr after use (not auto-cleared).
  static const char* _failNextWriteKey;
};
