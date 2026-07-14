#pragma once
#include "time_mock.h"
namespace sim {
class SimClock {
public:
  void advance(uint32_t ms) { _mockMillis += ms; }
  uint32_t now() const { return _mockMillis; }
};
} // namespace sim
