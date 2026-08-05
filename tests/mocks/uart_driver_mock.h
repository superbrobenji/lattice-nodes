// Mock of ESP-IDF's driver/uart.h — Phase I Task 5.
//
// Mirrors just enough of the real uart_driver surface for SerialAdapter's
// needs on host builds. Reads/writes route through the SAME in-memory
// buffers as serial_mock.h's global `Serial` object (rxQueue for reads,
// written for writes) rather than a separate buffer — this keeps the e2e
// harness's per-node Serial state swap (see tests/e2e/harness/NodeContext.cpp
// swapIn/swapOut, which snapshot/restore Serial.written/Serial.rxQueue)
// working unchanged, regardless of whether firmware code goes through
// Serial.read()/write() (Logger, unchanged) or uart_read_bytes/
// uart_write_bytes (SerialAdapter, migrated here).
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

typedef enum {
  UART_DATA_5_BITS,
  UART_DATA_6_BITS,
  UART_DATA_7_BITS,
  UART_DATA_8_BITS,
} uart_word_length_t;

typedef enum {
  UART_PARITY_DISABLE,
  UART_PARITY_EVEN,
  UART_PARITY_ODD,
} uart_parity_t;

typedef enum {
  UART_STOP_BITS_1,
  UART_STOP_BITS_1_5,
  UART_STOP_BITS_2,
} uart_stop_bits_t;

typedef enum {
  UART_HW_FLOWCTRL_DISABLE,
  UART_HW_FLOWCTRL_RTS,
  UART_HW_FLOWCTRL_CTS,
  UART_HW_FLOWCTRL_CTS_RTS,
} uart_hw_flowcontrol_t;

typedef enum {
  UART_SCLK_DEFAULT,
} uart_sclk_t;

typedef struct {
  int baud_rate;
  uart_word_length_t data_bits;
  uart_parity_t parity;
  uart_stop_bits_t stop_bits;
  uart_hw_flowcontrol_t flow_ctrl;
  uart_sclk_t source_clk;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, void* uart_queue, int intr_alloc_flags);
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t* uart_config);

// ticks_to_wait is accepted but ignored on host — reads are always
// non-blocking against the in-memory rxQueue (matches how SerialAdapter
// calls it: ticks_to_wait=0).
int uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length, int ticks_to_wait);
int uart_write_bytes(uart_port_t uart_num, const void* src, size_t size);

// Test helper — clears install-tracking state. Serial's own buffers are
// reset separately via Serial.reset() / NodeContext swap.
void resetUartDriverMock();
