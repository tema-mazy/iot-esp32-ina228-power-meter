#include "at_cmd.h"

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#include "link.h"
#include "logbuf.h"
#include "ota.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define AT_LINE_MAX 256

// Latest measurement, published by the gauge task. Snapshot-copied under the
// mutex and formatted outside it, so ATA never holds the lock while writing.
static ina228_reading_t s_reading;
static bool             s_reading_valid;
static SemaphoreHandle_t s_lock;

void at_publish(const ina228_reading_t *r, bool valid) {
  if (!s_lock)
    return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (valid)
    s_reading = *r;
  s_reading_valid = valid;
  xSemaphoreGive(s_lock);
}

static bool snapshot(ina228_reading_t *out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_reading;
  bool ok = s_reading_valid;
  xSemaphoreGive(s_lock);
  return ok;
}

// -- Responses ----------------------------------------------------------------

static void reply_ok(void) {
  link_write_str("OK\r\n");
  led_ok();
  ota_health_at_served();
}

static void reply_err(int code, const char *desc) {
  link_printf("ERROR %d %s\r\n", code, desc);
  led_error();
}

// -- Commands -----------------------------------------------------------------

static void cmd_ati(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  link_printf("%s,%s %s,esp32c3,%02X%02X%02X%02X%02X%02X\r\n", app->version,
              app->date, app->time, mac[0], mac[1], mac[2], mac[3], mac[4],
              mac[5]);
  led_ok();
}

static void cmd_ata(void) {
  ina228_reading_t r;
  if (!snapshot(&r)) {
    reply_err(AT_ERR_INA, "no valid reading");
    return;
  }
  // SoC fields are absent until the gauge lands in Phase 5. Emitting them as
  // nulls now would let a host bind to values that do not yet mean anything.
  link_printf("{\"v\":%.4f,\"i\":%.5f,\"p\":%.4f,\"t\":%.1f,"
              "\"q_c\":%.3f,\"e_j\":%.2f,\"err\":%u}\r\n",
              r.bus_v, r.current_a, r.power_w, r.temp_c, r.charge_c, r.energy_j,
              (unsigned)(r.diag & (INA228_DIAG_ENERGYOF | INA228_DIAG_CHARGEOF |
                                   INA228_DIAG_MATHOF)));
  led_ok();
}

static void cmd_atl(void) {
  int n = logbuf_count();
  link_printf("%d lines follow\r\n", n);
  char line[LOGBUF_LINE_MAX];
  for (int i = 0; i < n; i++) {
    if (logbuf_get(i, line, sizeof(line)) == 0)
      link_printf("%s\r\n", line);
  }
  reply_ok();
}

// ATFW=<bytes>,<md5hex>
static void cmd_atfw(const char *args) {
  unsigned long total = 0;
  char          md5hex[40] = {0};

  if (sscanf(args, "%lu,%39s", &total, md5hex) != 2) {
    reply_err(AT_ERR_SYNTAX, "expected ATFW=<bytes>,<md5hex>");
    return;
  }
  if (strlen(md5hex) != 32) {
    reply_err(AT_ERR_PARAM, "md5 must be 32 hex chars");
    return;
  }
  uint8_t expect[16];
  for (int i = 0; i < 16; i++) {
    unsigned byte;
    if (sscanf(md5hex + i * 2, "%2x", &byte) != 1) {
      reply_err(AT_ERR_PARAM, "md5 not hex");
      return;
    }
    expect[i] = (uint8_t)byte;
  }

  // Everything rejectable must be rejected BEFORE the acknowledgement: OK
  // commits the host to streaming binary immediately, so an error after it
  // would be read as a response to a transfer that never started.
  esp_err_t pre = ota_check_size(total);
  if (pre == ESP_ERR_INVALID_SIZE) {
    reply_err(AT_ERR_PARAM, "size does not fit the OTA partition");
    return;
  }
  if (pre != ESP_OK) {
    reply_err(AT_ERR_OTA, "no OTA partition");
    return;
  }

  // Announce the chunk size so the host paces itself to whatever this build
  // uses, rather than both sides hardcoding a constant that could drift.
  link_printf("OK %d\r\n", OTA_CHUNK);
  led_ok();
  ota_health_at_served();
  // From here on every byte is firmware payload, not commands.

  esp_err_t err = ota_receive(total, expect);

  // Any failure may leave unread payload queued. Without this the next
  // read would treat firmware bytes as a command line.
  if (err != ESP_OK)
    link_flush_rx();

  switch (err) {
  case ESP_OK:
    reply_ok();
    vTaskDelay(pdMS_TO_TICKS(200)); // drain the response before rebooting
    esp_restart();
    break;
  case ESP_ERR_TIMEOUT:
    reply_err(AT_ERR_TIMEOUT, "transfer timed out, image unchanged");
    break;
  case ESP_ERR_INVALID_CRC:
    reply_err(AT_ERR_CHECKSUM, "md5 mismatch, image rejected");
    break;
  case ESP_ERR_INVALID_SIZE:
    reply_err(AT_ERR_PARAM, "size does not fit the OTA partition");
    break;
  default:
    reply_err(AT_ERR_OTA, esp_err_to_name(err));
    break;
  }
}

static void cmd_atz(void) {
  reply_ok();
  vTaskDelay(pdMS_TO_TICKS(100)); // let the response drain before the reset
  esp_restart();
}

// -- Dispatch -----------------------------------------------------------------

static void handle_line(char *line) {
  while (*line && isspace((unsigned char)*line))
    line++; // tolerate leading whitespace
  int len = strlen(line);
  while (len > 0 && isspace((unsigned char)line[len - 1]))
    line[--len] = '\0';
  if (len == 0)
    return; // bare newline: ignore, do not answer

  for (int i = 0; i < len; i++)
    line[i] = toupper((unsigned char)line[i]);

  if (strncmp(line, "AT", 2) != 0) {
    reply_err(AT_ERR_SYNTAX, "expected AT prefix");
    return;
  }

  const char *cmd = line + 2;

  if (*cmd == '\0')       { reply_ok();  return; } // AT
  if (!strcmp(cmd, "I"))  { cmd_ati();   return; }
  if (!strcmp(cmd, "A"))  { cmd_ata();   return; }
  if (!strcmp(cmd, "L"))  { cmd_atl();   return; }
  if (!strcmp(cmd, "Z"))  { cmd_atz();   return; }
  if (!strncmp(cmd, "FW=", 3)) { cmd_atfw(cmd + 3); return; }

  // Phase 4/5/6 commands, deliberately reported as not-yet-implemented rather
  // than unknown, so a host can tell "wrong firmware version" from "typo".
  if (!strncmp(cmd, "S", 1) || !strncmp(cmd, "R", 1) ||
      !strncmp(cmd, "C=", 2)) {
    reply_err(AT_ERR_UNKNOWN, "not implemented in this phase");
    return;
  }

  reply_err(AT_ERR_UNKNOWN, "unknown command");
}

void at_task(void *arg) {
  (void)arg;
  s_lock = xSemaphoreCreateMutex();

  char line[AT_LINE_MAX];
  int  pos = 0;

  while (1) {
    uint8_t ch;
    int n = link_read(&ch, 1, 100);
    if (n <= 0)
      continue;

    // Dispatch on LF only, and drop CR entirely. Dispatching on CR would
    // leave the LF of a CRLF pair queued -- and after ATFW that stray byte
    // becomes the first byte of the firmware image, shifting the whole
    // transfer and failing header validation.
    if (ch == '\r')
      continue;

    if (ch == '\n') {
      if (pos > 0) {
        line[pos] = '\0';
        handle_line(line);
        pos = 0;
      }
      continue;
    }

    if (pos < AT_LINE_MAX - 1) {
      line[pos++] = (char)ch;
    } else {
      // Overlong line: discard and report, rather than silently truncating
      // into a command that was never sent.
      pos = 0;
      reply_err(AT_ERR_SYNTAX, "line too long");
    }
  }
}
