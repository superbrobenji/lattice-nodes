#include "EepromCore.h"
#include "src/error/Error.h"
#include <cstdio>
#include <cstring>

namespace lattice {
namespace eeprom {

using namespace lattice::utils;

namespace {
detail::State _state;
} // namespace

namespace core_internal {

bool ensureInitialized() {
  if (!_state.isInitialized) {
    LATTICE_LOGLN("NVS", "NVS not initialized", lattice::utils::LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void logOperation(const char* operation, const char* details) {
  if (details) {
    LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_DEBUG, "%s: %s", operation, details);
  } else {
    LATTICE_LOGLN("NVS", operation, lattice::utils::LogLevel::LOG_DEBUG);
  }
}

uint16_t crc16(const uint8_t* data, size_t len) {
  // Phase I Task 4 (opportunistic UU): NOT swapped to esp_rom_crc16_le — see
  // Phase A audit finding 25 for the re-confirmed reasoning (not a bit-exact
  // match without an unverified transform; value is self-referential only).
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

bool persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant) {
  if (got == want) {
    return true;
  }
  if (securityRelevant) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 5,
                       "NVS write failed (security-relevant key)");
    return false; // unreachable outside UNIT_TEST
  }
  LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_ERROR, "write failed key=%s got=%u want=%u",
               key, (unsigned)got, (unsigned)want);
  return false;
}

esp_err_t nvsOpenRW(nvs_handle_t& handle) {
  return nvs_open(NVS_KEYS::NAMESPACE, NVS_READWRITE, &handle);
}

uint8_t nvsGetU8(const char* key, uint8_t defaultValue) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return defaultValue;
  }
  uint8_t value = defaultValue;
  esp_err_t err = nvs_get_u8(h, key, &value);
  nvs_close(h);
  return (err == ESP_OK) ? value : defaultValue;
}

size_t nvsPutU8(const char* key, uint8_t value) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_u8(h, key, value);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? sizeof(uint8_t) : 0;
}

uint32_t nvsGetU32(const char* key, uint32_t defaultValue) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return defaultValue;
  }
  uint32_t value = defaultValue;
  esp_err_t err = nvs_get_u32(h, key, &value);
  nvs_close(h);
  return (err == ESP_OK) ? value : defaultValue;
}

size_t nvsPutU32(const char* key, uint32_t value) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_u32(h, key, value);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? sizeof(uint32_t) : 0;
}

bool nvsGetBool(const char* key, bool defaultValue) {
  return nvsGetU8(key, defaultValue ? 1 : 0) != 0;
}

size_t nvsPutBool(const char* key, bool value) {
  return nvsPutU8(key, value ? 1 : 0);
}

size_t nvsGetBytes(const char* key, void* buf, size_t maxLen) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return 0;
  }
  size_t len = maxLen;
  esp_err_t err = nvs_get_blob(h, key, buf, &len);
  nvs_close(h);
  return (err == ESP_OK) ? len : 0;
}

size_t nvsPutBytes(const char* key, const void* buf, size_t len) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_blob(h, key, buf, len);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? len : 0;
}

bool nvsRemove(const char* key) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return false;
  }
  err = nvs_erase_key(h, key);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err == ESP_OK;
}

bool nvsHasKey(const char* key) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return false;
  }
  size_t size = 0;
  esp_err_t err = nvs_get_blob(h, key, nullptr, &size);
  nvs_close(h);
  return err == ESP_OK;
}

void peerKey(uint8_t index, char* out, size_t outSize) {
  snprintf(out, outSize, "peer%u", static_cast<unsigned>(index));
}

bool isDevModeInternal() {
  return _state.isDevMode;
}

uint32_t& devEpochRef() {
  return _state.devEpoch;
}

} // namespace core_internal

namespace {
void nvsClearAll() {
  nvs_handle_t h;
  esp_err_t err = core_internal::nvsOpenRW(h);
  if (err != ESP_OK) {
    return;
  }
  err = nvs_erase_all(h);
  if (err == ESP_OK) {
    nvs_commit(h);
  }
  nvs_close(h);
}
} // namespace

bool init() {
  if (_state.isInitialized)
    return true;
  nvs_handle_t probe;
  esp_err_t err = nvs_open(NVS_KEYS::NAMESPACE, NVS_READWRITE, &probe);
  if (err != ESP_OK) {
    LATTICE_LOGLN("NVS", "Failed to open NVS namespace", lattice::utils::LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromCore: NVS begin failed");
    return false;
  }
  nvs_close(probe);
  _state.isInitialized = true;

  uint8_t reason = core_internal::nvsGetU8(NVS_KEYS::REBOOT_REASON, 0xFF);
  if (reason == 0x00) {
    size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_REASON, 0xFF);
    core_internal::persistOrEscalate(NVS_KEYS::REBOOT_REASON, n, sizeof(uint8_t), false);
  }
  uint8_t count = core_internal::nvsGetU8(NVS_KEYS::REBOOT_COUNT, 0);
  if (count > 10) {
    size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_COUNT, 0);
    core_internal::persistOrEscalate(NVS_KEYS::REBOOT_COUNT, n, sizeof(uint8_t), false);
  }

  core_internal::logOperation("Initialized", "NVS ready");
  return true;
}

void setDevMode(bool devMode) {
  _state.isDevMode = devMode;
  core_internal::logOperation("Dev mode set",
                              devMode ? "Development mode enabled" : "Production mode enabled");
}

bool getDevMode() {
  return _state.isDevMode;
}

void clearAll() {
  if (!core_internal::ensureInitialized())
    return;
  nvsClearAll();
  core_internal::logOperation("All NVS cleared");
}

void dumpEEPROM() {
  LATTICE_LOGLN("NVS", "NVS dump not implemented (use idf.py nvs-dump)",
                lattice::utils::LogLevel::LOG_INFO);
}

#ifdef UNIT_TEST
bool isInitializedForTest() {
  return _state.isInitialized;
}

detail::State& debugStateForTest() {
  return _state;
}
#endif

} // namespace eeprom
} // namespace lattice
