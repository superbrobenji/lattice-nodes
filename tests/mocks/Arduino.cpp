#include "Arduino.h"
ESPClass ESP;
int lattice_test_errFailCount = 0;
int _mockDigitalPinState[MOCK_DIGITAL_PIN_COUNT] = {0};
int _mockDigitalWriteCallCount = 0;

// Definition for the Arduino.h declaration above — see that header's comment
// for why this can't be `inline` in the header itself.
uint32_t esp_random() {
  return 42;
}
