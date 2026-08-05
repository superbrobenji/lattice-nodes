#include "Arduino.h"
ESPClass ESP;
int lattice_test_errFailCount = 0;
int _mockDigitalPinState[MOCK_DIGITAL_PIN_COUNT] = {0};
int _mockDigitalWriteCallCount = 0;
