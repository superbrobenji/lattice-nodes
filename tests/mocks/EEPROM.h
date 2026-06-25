#pragma once
#include <cstdint>
#include <array>
#include <functional>

class EEPROMClass {
public:
  static constexpr size_t SIZE = 512;
  std::array<uint8_t, SIZE> _data{};
  int _commitCount = 0;
  bool _failOnCommit = false;

  bool begin(size_t) { return true; }
  void end() {}  // no-op in tests

  uint8_t read(int addr) {
    if (addr < 0 || addr >= (int)SIZE) return 0xFF;
    return _data[addr];
  }

  void write(int addr, uint8_t val) {
    if (addr >= 0 && addr < (int)SIZE) _data[addr] = val;
  }

  bool commit() {
    if (_failOnCommit) return false;
    ++_commitCount;
    return true;
  }

  uint8_t& operator[](int addr) { return _data[addr]; }

  // Test helpers
  void reset() { _data.fill(0xFF); _commitCount = 0; _failOnCommit = false; }
  int commitCount() const { return _commitCount; }
};

extern EEPROMClass EEPROM;
