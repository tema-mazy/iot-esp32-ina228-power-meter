#include "sdkconfig.h"
#if CONFIG_PM_LINK_UART

#include "link.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

#define LINK_UART UART_NUM_1

esp_err_t link_init(void) {
  uart_config_t cfg = {
      .baud_rate = CONFIG_PM_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  esp_err_t err = uart_driver_install(LINK_UART, 1024, 2048, 0, NULL, 0);
  if (err != ESP_OK) return err;
  err = uart_param_config(LINK_UART, &cfg);
  if (err != ESP_OK) return err;
  // GPIO14/15 are SPI flash pins on the C3 and cannot be used; those numbers
  // in the original notes refer to the Raspberry Pi header (hardware.md S4.2).
  return uart_set_pin(LINK_UART, CONFIG_PM_UART_TX_GPIO, CONFIG_PM_UART_RX_GPIO,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

int link_read(uint8_t *buf, size_t len, uint32_t timeout_ms) {
  return uart_read_bytes(LINK_UART, buf, len, pdMS_TO_TICKS(timeout_ms));
}

void link_flush_rx(void) { uart_flush_input(LINK_UART); }

int link_write(const uint8_t *buf, size_t len) {
  return uart_write_bytes(LINK_UART, buf, len);
}

#endif
