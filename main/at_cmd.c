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
#include "config.h"
#include "gauge.h"
#include "ota.h"
#include "storage.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT_LINE_MAX 256

// Latest measurement, published by the gauge task. Snapshot-copied under the
// mutex and formatted outside it, so ATA never holds the lock while writing.
static ina228_reading_t s_reading;
static bool             s_reading_valid;
static SemaphoreHandle_t s_lock;

// Active battery config. NULL-equivalent when unprovisioned: the device is
// still a perfectly good voltmeter/ammeter without it, only SoC needs it.
static battery_config_t s_cfg;
static bool             s_cfg_valid;

bool at_get_config(battery_config_t *out) {
  if (!s_cfg_valid)
    return false;
  if (out)
    *out = s_cfg;
  return true;
}

void at_set_config(const battery_config_t *cfg) {
  s_cfg = *cfg;
  s_cfg_valid = true;
}

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
  unsigned err = (unsigned)(r.diag & (INA228_DIAG_ENERGYOF |
                                      INA228_DIAG_CHARGEOF |
                                      INA228_DIAG_MATHOF));
  gauge_info_t g;
  if (at_get_config(NULL) && gauge_get(&g)) {
    link_printf("{\"v\":%.4f,\"i\":%.5f,\"p\":%.4f,\"t\":%.1f,"
                "\"soc\":%.1f,\"mah_left\":%.0f,\"mah_used\":%.0f,"
                "\"wh\":%.2f,\"v_ocv\":%.4f,\"r\":%.3f,\"v_full\":%.3f,"
                "\"state\":\"%s\",\"est\":%s,"
                "\"q_c\":%.3f,\"e_j\":%.2f,\"err\":%u}\r\n",
                r.bus_v, r.current_a, r.power_w, r.temp_c, g.soc, g.mah_left,
                g.mah_used, g.wh_left, g.v_ocv, g.r_total, g.v_full_pc,
                gauge_mode_name(g.mode), g.est ? "true" : "false",
                r.charge_c, r.energy_j, err);
  } else {
    // Unprovisioned: still a working voltmeter and ammeter. SoC fields are
    // omitted rather than emitted as nulls, so a host cannot bind to values
    // that do not mean anything.
    link_printf("{\"v\":%.4f,\"i\":%.5f,\"p\":%.4f,\"t\":%.1f,"
                "\"q_c\":%.3f,\"e_j\":%.2f,\"err\":%u}\r\n",
                r.bus_v, r.current_a, r.power_w, r.temp_c, r.charge_c,
                r.energy_j, err);
  }
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

// ATS=<chem>,<xSyP>,<mAh>,<Vmin>,<Vmax>,<Imax>[,<pack_id>]
static void cmd_ats(const char *args) {
  battery_config_t cfg;
  const char      *bad = NULL;

  if (!config_parse(args, &cfg, &bad)) {
    reply_err(AT_ERR_PARAM, bad);
    return;
  }
  config_warn_if_odd(&cfg);

  // Apply before persisting: a calibration the hardware rejects (Imax x
  // Rshunt overflowing the 15-bit SHUNT_CAL) must not be stored as if it
  // worked, or every subsequent boot would fail the same way.
  if (ina228_set_calibration(CONFIG_PM_RSHUNT_MICROOHM, cfg.imax) != ESP_OK) {
    reply_err(AT_ERR_PARAM, "Imax x Rshunt overflows SHUNT_CAL");
    return;
  }

  esp_err_t err = storage_save_config(&cfg);
  if (err != ESP_OK) {
    reply_err(AT_ERR_NVS, esp_err_to_name(err));
    return;
  }

  at_set_config(&cfg);
  reply_ok();
}

static void cmd_ats_query(void) {
  battery_config_t cfg;
  if (!at_get_config(&cfg)) {
    reply_err(AT_ERR_NOCONFIG, "no battery provisioned, use ATS=");
    return;
  }
  char line[160];
  config_format(&cfg, line, sizeof(line));
  link_printf("%s\r\n", line);
  led_ok();
  ota_health_at_served();
}

// ATR: declare a battery swap and assume the new pack is full.
static void cmd_atr(void) {
  battery_config_t cfg;
  if (!at_get_config(&cfg)) {
    reply_err(AT_ERR_NOCONFIG, "no battery provisioned, use ATS=");
    return;
  }
  ina228_reading_t r;
  if (!snapshot(&r)) {
    reply_err(AT_ERR_INA, "no valid reading");
    return;
  }
  if (!gauge_reseed_full(&cfg, &r)) {
    reply_err(AT_ERR_PARAM, "voltage says the pack is not full");
    return;
  }
  reply_ok();
}

// ATC=<mAh>: force remaining capacity.
static void cmd_atc(const char *args) {
  battery_config_t cfg;
  if (!at_get_config(&cfg)) {
    reply_err(AT_ERR_NOCONFIG, "no battery provisioned, use ATS=");
    return;
  }
  char *end = NULL;
  float mah = strtof(args, &end);
  if (end == args || *end || mah < 0 || mah > (float)cfg.capacity_mah) {
    reply_err(AT_ERR_PARAM, "mAh (0..capacity)");
    return;
  }
  gauge_set_remaining(&cfg, mah);
  reply_ok();
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

  // Uppercase only up to the first '=' . Everything after it is data, and
  // pack_id is case-sensitive; chemistry names are compared case-insensitively
  // anyway.
  for (int i = 0; i < len && line[i] != '='; i++)
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
  if (!strcmp(cmd, "S?"))      { cmd_ats_query();   return; }
  if (!strncmp(cmd, "S=", 2))  { cmd_ats(cmd + 2);  return; }
  if (!strcmp(cmd, "R"))       { cmd_atr();         return; }
  if (!strncmp(cmd, "C=", 2))  { cmd_atc(cmd + 2);  return; }

  // Phase 4/5/6 commands, deliberately reported as not-yet-implemented rather
  // than unknown, so a host can tell "wrong firmware version" from "typo".
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
