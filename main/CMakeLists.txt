idf_component_register(
  SRCS "main.c" "ina228.c" "led.c" "logbuf.c" "at_cmd.c" "ota.c"
       "config.c" "storage.c" "gauge.c"
       "link_common.c" "link_usb.c" "link_uart.c"
  INCLUDE_DIRS "."
  REQUIRES driver esp_driver_i2c esp_driver_uart esp_app_format app_update
           esp_hw_support mbedtls nvs_flash
)
