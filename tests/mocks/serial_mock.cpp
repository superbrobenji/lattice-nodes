#include "serial_mock.h"
SerialClass Serial;

void SerialClass::print(int v, int base) {
  if (base == 16) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%X", v);
    output += buf;
  } else {
    output += std::to_string(v);
  }
}
