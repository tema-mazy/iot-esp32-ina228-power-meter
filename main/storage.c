#include "storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "storage";

#define NS         "battery"
#define KEY_ACTIVE "active"

esp_err_t storage_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // A truncated partition or a version bump. Erasing loses the stored
    // config, which the gauge then re-seeds from OCV on the next boot -- much
    // better than refusing to start.
    ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

esp_err_t storage_save_config(const battery_config_t *cfg) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK)
    return err;

  err = nvs_set_blob(h, cfg->pack_id, cfg, sizeof(*cfg));
  if (err == ESP_OK)
    err = nvs_set_str(h, KEY_ACTIVE, cfg->pack_id);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);

  if (err == ESP_OK)
    ESP_LOGI(TAG, "saved pack '%s'", cfg->pack_id);
  return err;
}

esp_err_t storage_load_pack(const char *pack_id, battery_config_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
  if (err != ESP_OK)
    return err;

  size_t len = sizeof(*out);
  err = nvs_get_blob(h, pack_id, out, &len);
  nvs_close(h);
  if (err != ESP_OK)
    return err;

  if (len != sizeof(*out) || out->version != CFG_VERSION) {
    // Written by a different firmware layout. Treat as absent rather than
    // trusting fields that may have moved.
    ESP_LOGW(TAG, "pack '%s' has config version %u (want %u), ignoring",
             pack_id, out->version, CFG_VERSION);
    return ESP_ERR_NVS_NOT_FOUND;
  }
  return ESP_OK;
}

esp_err_t storage_load_config(battery_config_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
  if (err != ESP_OK)
    return err;

  char pack_id[CFG_PACK_ID_MAX];
  size_t len = sizeof(pack_id);
  err = nvs_get_str(h, KEY_ACTIVE, pack_id, &len);
  nvs_close(h);
  if (err != ESP_OK)
    return err;

  return storage_load_pack(pack_id, out);
}
