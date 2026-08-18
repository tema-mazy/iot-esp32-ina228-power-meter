#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define LED_GPIO ((gpio_num_t)CONFIG_PM_LED_GPIO)

void led_init(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << LED_GPIO,
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  led_set(false);
}

void led_set(bool on) {
#if CONFIG_PM_LED_ACTIVE_LOW
  gpio_set_level(LED_GPIO, on ? 0 : 1);
#else
  gpio_set_level(LED_GPIO, on ? 1 : 0);
#endif
}

void led_blink(int count, int on_ms, int off_ms) {
  for (int i = 0; i < count; i++) {
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_set(false);
    if (off_ms)
      vTaskDelay(pdMS_TO_TICKS(off_ms));
  }
}

// A flicker, not a blink. A host polling ATA at 1 Hz would turn any longer
// pattern into a steady 1 Hz blink - indistinguishable from the INA228 fault
// indication, which is the one pattern that must stay unmistakable.
void led_ok(void) {
#if CONFIG_PM_LED_ACTIVITY
  led_blink(1, 20, 0);
#endif
}

void led_error(void) {
#if CONFIG_PM_LED_ACTIVITY
  led_blink(3, 60, 100);
#endif
}

void led_soc(int soc) {
  led_blink(2, 60, 100);            // ack
  vTaskDelay(pdMS_TO_TICKS(300));
  int n = (soc + 5) / 10;
  if (n < 1) {
    led_blink(1, 1000, 0);          // < 5%: one long blink
    return;
  }
  if (n > 10) n = 10;
  led_blink(n, 300, 300);
}
