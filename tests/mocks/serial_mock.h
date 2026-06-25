#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <cstdarg>
#include <cstdio>

class SerialClass {
public:
  std::vector<uint8_t> written;
  std::string          output;
  int                  _baudRate = 0;

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
  int available()            { return 0; }
  int read()                 { return -1; }
  void flush()               {}

  // vprintf stub for Logger compatibility
  void vprintf(const char* fmt, va_list args) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    output += buf;
  }

  void reset() { written.clear(); output.clear(); }
};

extern SerialClass Serial;
