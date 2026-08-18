// Universal battery monitor - ESP32-C3 + INA228.
// Phase 2/3: transport, log ring buffer, AT core. Gauge is Phase 5.
// See DEVELOPMENT_PLAN.md for the phased build.

#include "at_cmd.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ina228.h"
#include "led.h"
#include "link.h"
#include "logbuf.h"
#include "ota.h"
#include "sdkconfig.h"

static const char *TAG = "monitor";

static const char *reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_POWERON:   return "power-on"; // battery connected -> maybe a swap
  case ESP_RST_SW:        return "software"; // ATZ or OTA reboot
  case ESP_RST_PANIC:     return "panic";
  case ESP_RST_INT_WDT:   return "int watchdog";
  case ESP_RST_TASK_WDT:  return "task watchdog";
  case ESP_RST_BROWNOUT:  return "brownout";
  case ESP_RST_DEEPSLEEP: return "deep sleep";
  default:                return "other";
  }
}

static void log_identity(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  esp_chip_info_t chip;
  esp_chip_info(&chip);

  ESP_LOGI(TAG, "battery monitor %s (%s %s)", app->version, app->date, app->time);
  ESP_LOGI(TAG, "ESP32-C3 rev %d.%d, %" PRIu32 " KB heap", chip.revision / 100,
           chip.revision % 100, esp_get_free_heap_size() / 1024);

  // Power-on means the battery was (re)connected, which the gauge treats as a
  // possible swap. Distinguishing it from software resets is the whole basis
  // of the re-seed logic (plan S4).
  esp_reset_reason_t rr = esp_reset_reason();
  ESP_LOGI(TAG, "reset: %s%s", reset_reason_str(rr),
           rr == ESP_RST_POWERON ? " (battery connected)" : "");

  const esp_partition_t *run = esp_ota_get_running_partition();
  ESP_LOGI(TAG, "running from %s @ 0x%" PRIx32, run->label, run->address);
  ESP_LOGI(TAG, "pins SDA=%d SCL=%d LED=%d BTN=%d", CONFIG_PM_I2C_SDA_GPIO,
           CONFIG_PM_I2C_SCL_GPIO, CONFIG_PM_LED_GPIO, CONFIG_PM_BUTTON_GPIO);
}

// -- Gauge task ---------------------------------------------------------------
// Owns the INA228 and the coulomb count. Deliberately independent of the AT
// task: link I/O must never be able to stall measurement (plan S5).

static void gauge_task(void *arg) {
  (void)arg;

  // Imax is provisioned by ATS in Phase 4; until then assume the shunt's full
  // range so bench readings are valid with no configuration.
  ina228_config_t cfg = {
      .sda_gpio    = CONFIG_PM_I2C_SDA_GPIO,
      .scl_gpio    = CONFIG_PM_I2C_SCL_GPIO,
      .addr        = CONFIG_PM_INA_I2C_ADDR,
      .freq_hz     = CONFIG_PM_I2C_FREQ_HZ,
      .rshunt_uohm = CONFIG_PM_RSHUNT_MICROOHM,
      .imax_a      = 10.0f,
  };

  bool ok = (ina228_init(&cfg) == ESP_OK);
  if (ok)
    ota_health_ina_ok();
  else
    ESP_LOGE(TAG, "INA228 init failed - check I2C wiring and the QT red wire");

  while (1) {
    if (!ok) {
      at_publish(NULL, false);
      // Continuous 0.5 s blink = INA228 fault. The only unprompted pattern,
      // and the only repeating one (plan S2.6).
      led_set(true);
      vTaskDelay(pdMS_TO_TICKS(500));
      led_set(false);
      vTaskDelay(pdMS_TO_TICKS(500));
      ok = (ina228_init(&cfg) == ESP_OK);
      if (ok) {
        ESP_LOGW(TAG, "INA228 recovered");
        ota_health_ina_ok();
      }
      continue;
    }

    ina228_reading_t r;
    if (ina228_read(&r) != ESP_OK) {
      ESP_LOGE(TAG, "INA228 read failed");
      ok = false;
      continue;
    }
    at_publish(&r, true);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// -- app_main -----------------------------------------------------------------

void app_main(void) {
  led_init();
  logbuf_init(); // divert ESP_LOG before anything logs
  log_identity();

  if (link_init() != ESP_OK) {
    // Nothing can report this: the link is how reporting happens. Blink and
    // reboot rather than sit silently dead.
    led_blink(10, 60, 60);
    esp_restart();
  }

  led_blink(3, 80, 120); // firmware is running

  xTaskCreate(gauge_task, "gauge", 4096, NULL, 5, NULL);
  xTaskCreate(at_task, "at", 4096, NULL, 4, NULL);

  ESP_LOGI(TAG, "ready: AT link up, gauge polling at 1 Hz");
  vTaskDelete(NULL);
}
