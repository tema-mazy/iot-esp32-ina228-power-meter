#include "sdkconfig.h"
#if CONFIG_PM_LINK_USB

#include "link.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

esp_err_t link_init(void) {
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  // Must exceed OTA_CHUNK (ota.h): the driver discards on RX-ring overflow,
  // and the OTA flow-control ACK only paces the host between chunks.
  cfg.rx_buffer_size = 4096;
  cfg.tx_buffer_size = 2048; // ATL emits many lines back to back
  return usb_serial_jtag_driver_install(&cfg);
}

int link_read(uint8_t *buf, size_t len, uint32_t timeout_ms) {
  return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(timeout_ms));
}

void link_flush_rx(void) {
  uint8_t scratch[64];
  while (usb_serial_jtag_read_bytes(scratch, sizeof(scratch), pdMS_TO_TICKS(50)) > 0)
    ;
}

int link_write(const uint8_t *buf, size_t len) {
  // Finite timeout, discard on expiry. Losing a response to a host that is not
  // listening is correct behaviour; blocking here would stall the gauge.
  return usb_serial_jtag_write_bytes(buf, len,
                                     pdMS_TO_TICKS(LINK_WRITE_TIMEOUT_MS));
}

#endif
