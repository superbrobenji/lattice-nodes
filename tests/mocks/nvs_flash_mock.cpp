#include "nvs.h"
#include "nvs_flash.h"
#include <cstring>

std::map<std::string, std::vector<uint8_t>> NvsMock::_store;
const char* NvsMock::_failNextWriteKey = nullptr;

namespace {

std::map<nvs_handle_t, std::string> g_handleNamespace;
nvs_handle_t g_nextHandle = 1;

std::string storeKey(const std::string& ns, const char* key) {
  return ns + "/" + (key ? key : "");
}

bool consumeFailHook(const char* key) {
  if (NvsMock::_failNextWriteKey && key && std::strcmp(NvsMock::_failNextWriteKey, key) == 0) {
    return true;
  }
  return false;
}

} // namespace

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t /*open_mode*/,
                   nvs_handle_t* out_handle) {
  if (!namespace_name || !out_handle) {
    return ESP_ERR_NVS_INVALID_NAME;
  }
  nvs_handle_t h = g_nextHandle++;
  g_handleNamespace[h] = namespace_name;
  *out_handle = h;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  g_handleNamespace.erase(handle);
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  if (g_handleNamespace.find(handle) == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  return ESP_OK; // mock store is written synchronously by the set_* calls
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end() || !out_value) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  auto it = NvsMock::_store.find(storeKey(nsIt->second, key));
  if (it == NvsMock::_store.end() || it->second.size() < sizeof(uint8_t)) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  *out_value = it->second[0];
  return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (consumeFailHook(key)) {
    return ESP_FAIL;
  }
  NvsMock::_store[storeKey(nsIt->second, key)] = std::vector<uint8_t>{value};
  return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out_value) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end() || !out_value) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  auto it = NvsMock::_store.find(storeKey(nsIt->second, key));
  if (it == NvsMock::_store.end() || it->second.size() < sizeof(uint32_t)) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  std::memcpy(out_value, it->second.data(), sizeof(uint32_t));
  return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (consumeFailHook(key)) {
    return ESP_FAIL;
  }
  std::vector<uint8_t> buf(sizeof(uint32_t));
  std::memcpy(buf.data(), &value, sizeof(uint32_t));
  NvsMock::_store[storeKey(nsIt->second, key)] = std::move(buf);
  return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end() || !length) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  auto it = NvsMock::_store.find(storeKey(nsIt->second, key));
  if (it == NvsMock::_store.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  size_t storedLen = it->second.size();
  if (!out_value) {
    // Query-only mode (real IDF semantics): report required size, no copy.
    *length = storedLen;
    return ESP_OK;
  }
  if (*length < storedLen) {
    *length = storedLen;
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  std::memcpy(out_value, it->second.data(), storedLen);
  *length = storedLen;
  return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (consumeFailHook(key)) {
    return ESP_FAIL;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(value);
  NvsMock::_store[storeKey(nsIt->second, key)] = std::vector<uint8_t>(bytes, bytes + length);
  return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  size_t erased = NvsMock::_store.erase(storeKey(nsIt->second, key));
  return erased > 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
  auto nsIt = g_handleNamespace.find(handle);
  if (nsIt == g_handleNamespace.end()) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  const std::string prefix = nsIt->second + "/";
  auto it = NvsMock::_store.begin();
  while (it != NvsMock::_store.end()) {
    if (it->first.rfind(prefix, 0) == 0) {
      it = NvsMock::_store.erase(it);
    } else {
      ++it;
    }
  }
  return ESP_OK;
}

esp_err_t nvs_flash_init(void) {
  return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
  NvsMock::_store.clear();
  return ESP_OK;
}
