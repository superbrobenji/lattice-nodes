// Mock ESP-IDF driver/gpio.h — Phase I Task 7 (GPIO natives: items MM/PP/QQ/RR).
//
// Backs gpio_set_level()/gpio_get_level() with the SAME _mockDigitalPinState
// array and _mockDigitalWriteCallCount counter that Arduino.h's
// digitalRead()/digitalWrite() mocks already used (see tests/mocks/Arduino.h)
// — existing tests that drive pin state via setMockDigitalRead() (Button
// debounce tests) or assert on _mockDigitalWriteCallCount (DisplayManager
// redraw-throttle tests) keep working unchanged now that production code
// calls the native gpio_* functions instead of the Arduino digitalWrite/
// digitalRead/pinMode wrappers.
#pragma once
#include <cstdint>
#include "esp_err.h"

typedef int gpio_num_t;

typedef enum {
  GPIO_MODE_DISABLE = 0,
  GPIO_MODE_INPUT = 1,
  GPIO_MODE_OUTPUT = 2,
  GPIO_MODE_OUTPUT_OD = 3,
  GPIO_MODE_INPUT_OUTPUT_OD = 5,
  GPIO_MODE_INPUT_OUTPUT = 6,
} gpio_mode_t;

typedef enum {
  GPIO_PULLUP_DISABLE = 0,
  GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
  GPIO_PULLDOWN_DISABLE = 0,
  GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef enum {
  GPIO_PULLUP_ONLY,
  GPIO_PULLDOWN_ONLY,
  GPIO_PULLUP_PULLDOWN,
  GPIO_FLOATING,
} gpio_pull_mode_t;

typedef enum {
  GPIO_INTR_DISABLE = 0,
  GPIO_INTR_POSEDGE = 1,
  GPIO_INTR_NEGEDGE = 2,
  GPIO_INTR_ANYEDGE = 3,
  GPIO_INTR_LOW_LEVEL = 4,
  GPIO_INTR_HIGH_LEVEL = 5,
} gpio_int_type_t;

typedef struct {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void*);

esp_err_t gpio_config(const gpio_config_t* cfg);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);

// Test helper — resets ISR-service-installed / handler-registration tracking
// (mirrors resetUartDriverMock()).
void resetGpioMock();
