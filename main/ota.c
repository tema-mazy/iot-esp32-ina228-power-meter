#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "link.h"
#include "mbedtls/md5.h"
#include <string.h>

static const char *TAG = "ota";

esp_err_t ota_check_size(size_t total) {
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (!part) {
    ESP_LOGE(TAG, "no OTA partition available");
    return ESP_ERR_NOT_FOUND;
  }
  if (total == 0 || total > part->size) {
    ESP_LOGE(TAG, "size %u outside partition %s (%" PRIu32 " bytes)",
             (unsigned)total, part->label, part->size);
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

esp_err_t ota_receive(size_t total, const uint8_t expect_md5[16]) {
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  esp_err_t pre = ota_check_size(total);
  if (pre != ESP_OK)
    return pre;

  esp_ota_handle_t h = 0;
  esp_err_t err = esp_ota_begin(part, total, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGW(TAG, "receiving %u bytes -> %s", (unsigned)total, part->label);

  mbedtls_md5_context md5;
  mbedtls_md5_init(&md5);
  mbedtls_md5_starts(&md5);

  // Static: OTA_CHUNK is far too large for the AT task's stack.
  static uint8_t buf[OTA_CHUNK];
  size_t got = 0;

  while (got < total) {
    size_t want = total - got;
    if (want > OTA_CHUNK)
      want = OTA_CHUNK;

    size_t have = 0;
    while (have < want) {
      int n = link_read(buf + have, want - have, OTA_RX_TIMEOUT_MS);
      if (n <= 0) {
        ESP_LOGE(TAG, "timeout after %u/%u bytes", (unsigned)(got + have),
                 (unsigned)total);
        mbedtls_md5_free(&md5);
        esp_ota_abort(h);
        return ESP_ERR_TIMEOUT;
      }
      have += n;
    }

    mbedtls_md5_update(&md5, buf, have);
    err = esp_ota_write(h, buf, have);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
      mbedtls_md5_free(&md5);
      esp_ota_abort(h);
      return err;
    }
    got += have;

    // Chunk consumed and committed: invite the next one. Without this the
    // host overruns the RX ring and the overflow is discarded silently.
    const uint8_t ack = OTA_CHUNK_ACK;
    link_write(&ack, 1);
  }

  uint8_t actual[16];
  mbedtls_md5_finish(&md5, actual);
  mbedtls_md5_free(&md5);

  // Verify before esp_ota_end so a corrupted transfer never reaches the
  // bootloader's validation, let alone the boot partition.
  if (memcmp(actual, expect_md5, 16) != 0) {
    ESP_LOGE(TAG, "MD5 mismatch, image rejected");
    esp_ota_abort(h);
    return ESP_ERR_INVALID_CRC;
  }

  err = esp_ota_end(h); // also checks the image header
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGW(TAG, "image accepted, booting %s", part->label);
  return ESP_OK;
}

// -- Rollback health gate -----------------------------------------------------

static bool s_ina_ok, s_at_served, s_confirmed;

static void confirm_if_healthy(void) {
  if (s_confirmed || !s_ina_ok || !s_at_served)
    return;

  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
      ESP_LOGW(TAG, "image confirmed valid (INA228 ok + host served)");
  }
  s_confirmed = true;
}

void ota_health_ina_ok(void)    { s_ina_ok = true;    confirm_if_healthy(); }
void ota_health_at_served(void) { s_at_served = true; confirm_if_healthy(); }
