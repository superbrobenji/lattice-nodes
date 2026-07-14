#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <cstdarg>
#include <cstdio>
#include <deque>

class SerialClass {
public:
  std::vector<uint8_t> written;
  std::string          output;
  int                  _baudRate = 0;
  std::deque<uint8_t> rxQueue;

  void begin(int baud) { _baudRate = baud; }
  void print(const char* s)  { if (s) output += s; }
  void print(const std::string& s) { output += s; }
  void print(int v)          { output += std::to_string(v); }
  void print(int v, int base);
  void print(unsigned int v)       { output += std::to_string(v); }
  void print(long v)               { output += std::to_string(v); }
  void print(unsigned long v)      { output += std::to_string(v); }
  void print(uint8_t v)            { output += std::to_string(static_cast<unsigned int>(v)); }
  void println(const char* s = "") { if (s) output += s; output += '\n'; }
  void println(const std::string& s) { output += s; output += '\n'; }
  void println(int v)        { output += std::to_string(v); output += '\n'; }
  size_t write(const uint8_t* buf, size_t n) { written.insert(written.end(), buf, buf+n); return n; }
  size_t write(uint8_t b)    { written.push_back(b); return 1; }
  void injectRx(const uint8_t* data, size_t n) { rxQueue.insert(rxQueue.end(), data, data + n); }
  int available() { return static_cast<int>(rxQueue.size()); }
  int read() {
    if (rxQueue.empty()) return -1;
    uint8_t b = rxQueue.front();
    rxQueue.pop_front();
    return b;
  }
  void flush()               {}

  // vprintf stub for Logger compatibility
  void vprintf(const char* fmt, va_list args) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    output += buf;
  }

  // printf for SIMULATE_MODE dump uses Serial.printf
  void printf(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
  }

  void reset() { written.clear(); output.clear(); rxQueue.clear(); }
};

extern SerialClass Serial;
