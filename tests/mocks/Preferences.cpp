#include "Preferences.h"
#include <cstring>

// Static storage definition
std::map<std::string, std::vector<uint8_t>> Preferences::_store;
const char* Preferences::_shortWriteKey = nullptr;

bool Preferences::begin(const char* name, bool readOnly) {
  _namespace = name ? name : "";
  return true;
}

void Preferences::end() {
  _namespace.clear();
}

std::string Preferences::_nsKey(const char* key) const {
  return _namespace + "/" + (key ? key : "");
}

bool Preferences::clear() {
  // Remove all keys with this namespace prefix
  auto it = _store.begin();
  while (it != _store.end()) {
    if (it->first.rfind(_namespace + "/", 0) == 0) {
      it = _store.erase(it);
    } else {
      ++it;
    }
  }
  return true;
}

bool Preferences::remove(const char* key) {
  std::string nskey = _nsKey(key);
  return _store.erase(nskey) > 0;
}

bool Preferences::isKey(const char* key) {
  std::string nskey = _nsKey(key);
  return _store.find(nskey) != _store.end();
}

uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) {
  std::string nskey = _nsKey(key);
  auto it = _store.find(nskey);
  if (it == _store.end() || it->second.size() < 1) {
    return defaultValue;
  }
  return it->second[0];
}

uint32_t Preferences::getUInt(const char* key, uint32_t defaultValue) {
  std::string nskey = _nsKey(key);
  auto it = _store.find(nskey);
  if (it == _store.end() || it->second.size() < 4) {
    return defaultValue;
  }
  uint32_t val = 0;
  memcpy(&val, it->second.data(), 4);
  return val;
}

bool Preferences::getBool(const char* key, bool defaultValue) {
  std::string nskey = _nsKey(key);
  auto it = _store.find(nskey);
  if (it == _store.end() || it->second.size() < 1) {
    return defaultValue;
  }
  return it->second[0] != 0;
}

size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
  std::string nskey = _nsKey(key);
  auto it = _store.find(nskey);
  if (it == _store.end()) {
    return 0;
  }
  size_t len = std::min(maxLen, it->second.size());
  memcpy(buf, it->second.data(), len);
  return len;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
  if (_shortWriteKey && std::strcmp(key, _shortWriteKey) == 0) {
    return 0; // simulate short write
  }
  std::string nskey = _nsKey(key);
  _store[nskey] = std::vector<uint8_t>{value};
  return 1;
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
  if (_shortWriteKey && std::strcmp(key, _shortWriteKey) == 0) {
    return 0; // simulate short write
  }
  std::string nskey = _nsKey(key);
  std::vector<uint8_t> buf(4);
  memcpy(buf.data(), &value, 4);
  _store[nskey] = buf;
  return 4;
}

size_t Preferences::putBool(const char* key, bool value) {
  if (_shortWriteKey && std::strcmp(key, _shortWriteKey) == 0) {
    return 0; // simulate short write
  }
  std::string nskey = _nsKey(key);
  _store[nskey] = std::vector<uint8_t>{static_cast<uint8_t>(value ? 1 : 0)};
  return 1;
}

size_t Preferences::putBytes(const char* key, const void* buf, size_t len) {
  if (_shortWriteKey && std::strcmp(key, _shortWriteKey) == 0) {
    return 0; // simulate short write
  }
  std::string nskey = _nsKey(key);
  const uint8_t* bytes = static_cast<const uint8_t*>(buf);
  _store[nskey] = std::vector<uint8_t>(bytes, bytes + len);
  return len;
}
