#include "uart_driver_mock.h"
#include "serial_mock.h"

namespace {
bool g_installed = false;
}

esp_err_t uart_driver_install(uart_port_t /*uart_num*/, int /*rx_buffer_size*/,
                              int /*tx_buffer_size*/, int /*queue_size*/, void* /*uart_queue*/,
                              int /*intr_alloc_flags*/) {
  g_installed = true;
  return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t /*uart_num*/, const uart_config_t* uart_config) {
  if (uart_config)
    Serial._baudRate = uart_config->baud_rate;
  return ESP_OK;
}

int uart_read_bytes(uart_port_t /*uart_num*/, void* buf, uint32_t length, int /*ticks_to_wait*/) {
  if (!buf)
    return -1;
  uint8_t* out = static_cast<uint8_t*>(buf);
  uint32_t n = 0;
  while (n < length && !Serial.rxQueue.empty()) {
    out[n++] = Serial.rxQueue.front();
    Serial.rxQueue.pop_front();
  }
  return static_cast<int>(n);
}

int uart_write_bytes(uart_port_t /*uart_num*/, const void* src, size_t size) {
  if (!src)
    return -1;
  const uint8_t* in = static_cast<const uint8_t*>(src);
  Serial.written.insert(Serial.written.end(), in, in + size);
  return static_cast<int>(size);
}

void resetUartDriverMock() {
  g_installed = false;
}
