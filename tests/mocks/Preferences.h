#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <vector>

class Preferences {
public:
  // Static shared store across all instances
  static std::map<std::string, std::vector<uint8_t>> _store;

  // Test hook: when non-null, the next put*(key,...) call whose key matches
  // returns 0 (simulating a short/failed NVS write) instead of writing.
  static const char* _shortWriteKey;

  Preferences() = default;
  ~Preferences() = default;

  // Begin namespace session (readonly flag not used in mock)
  bool begin(const char* name, bool readOnly = false);

  // End session (no-op in mock)
  void end();

  // Clear all keys in current namespace
  bool clear();

  // Remove specific key
  bool remove(const char* key);

  // Check if key exists
  bool isKey(const char* key);

  // Get methods
  uint8_t getUChar(const char* key, uint8_t defaultValue = 0);
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0);
  bool getBool(const char* key, bool defaultValue = false);
  size_t getBytes(const char* key, void* buf, size_t maxLen);

  // Put methods (return bytes written or 0 on failure)
  size_t putUChar(const char* key, uint8_t value);
  size_t putUInt(const char* key, uint32_t value);
  size_t putBool(const char* key, bool value);
  size_t putBytes(const char* key, const void* buf, size_t len);

private:
  std::string _namespace;

  // Helper to build namespaced key
  std::string _nsKey(const char* key) const;
};
