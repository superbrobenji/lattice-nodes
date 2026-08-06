#include "driver/gpio.h"
#include "Arduino.h" // _mockDigitalPinState / _mockDigitalWriteCallCount

namespace {
bool g_isrServiceInstalled = false;
}

esp_err_t gpio_config(const gpio_config_t* /*cfg*/) {
  // Host tests don't need per-pin mode/pull bookkeeping — pin state is driven
  // directly via setMockDigitalRead()/gpio_set_level(), independent of
  // whatever mode a gpio_config_t call would have configured on real
  // hardware.
  return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t /*pin*/, uint32_t /*level*/) {
  // Deliberately does NOT touch _mockDigitalPinState — mirrors Arduino.h's
  // digitalWrite() mock, which only counts calls and leaves pin state
  // decoupled from what was written (only setMockDigitalRead()/gpio_get_level
  // reads/writes it). SevenSegDisplay's ACK-wait loop in writeByte() writes
  // the last data bit to DIO via gpio_set_level() right before switching DIO
  // to input and polling gpio_get_level() for the chip's ACK — if
  // gpio_set_level() fed that write back into pin state, the ACK read would
  // see whatever bit was last written instead of the test's simulated ACK
  // (default LOW), and since the host's mock clock (time_mock.h) only
  // advances via explicit advanceMillis() calls, the bounded 20ms wait in
  // that loop would busy-spin forever.
  ++_mockDigitalWriteCallCount;
  return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) {
  return digitalRead(pin);
}

esp_err_t gpio_set_direction(gpio_num_t /*pin*/, gpio_mode_t /*mode*/) {
  return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t /*pin*/, gpio_pull_mode_t /*pull*/) {
  return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t /*pin*/, gpio_int_type_t /*type*/) {
  return ESP_OK;
}

esp_err_t gpio_install_isr_service(int /*flags*/) {
  g_isrServiceInstalled = true;
  return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t /*pin*/, gpio_isr_t /*handler*/, void* /*arg*/) {
  return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t /*pin*/) {
  return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t /*pin*/) {
  return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t /*pin*/) {
  return ESP_OK;
}

void resetGpioMock() {
  g_isrServiceInstalled = false;
}
