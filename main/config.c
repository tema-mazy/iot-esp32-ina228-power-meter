#include "config.h"

#include "esp_log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "config";

static const char *CHEM_NAMES[CHEM_COUNT] = {"LifePo", "LiIon", "AGM", "Acid"};

// Plausible charge-ceiling volts per cell, used only for the warning.
static const float CHEM_VMAX_LO[CHEM_COUNT] = {3.50f, 4.00f, 2.30f, 2.30f};
static const float CHEM_VMAX_HI[CHEM_COUNT] = {3.75f, 4.25f, 2.50f, 2.50f};

const char *chem_name(chem_t c) {
  return (c < CHEM_COUNT) ? CHEM_NAMES[c] : "?";
}

bool chem_from_name(const char *s, chem_t *out) {
  for (int i = 0; i < CHEM_COUNT; i++) {
    if (strcasecmp(s, CHEM_NAMES[i]) == 0) {
      *out = (chem_t)i;
      return true;
    }
  }
  return false;
}

// Splits on commas in place. Returns the field count.
static int split(char *s, char **fields, int max) {
  int n = 0;
  char *save = NULL;
  for (char *tok = strtok_r(s, ",", &save); tok && n < max;
       tok = strtok_r(NULL, ",", &save)) {
    while (*tok == ' ')
      tok++;
    fields[n++] = tok;
  }
  return n;
}

// "5S3P" -> series 5, parallel 3
static bool parse_sp(const char *s, uint8_t *series, uint8_t *parallel) {
  int ser = 0, par = 0;
  char c1 = 0, c2 = 0;
  if (sscanf(s, "%d%c%d%c", &ser, &c1, &par, &c2) != 4)
    return false;
  if (toupper((unsigned char)c1) != 'S' || toupper((unsigned char)c2) != 'P')
    return false;
  if (ser < 1 || ser > 99 || par < 1 || par > 99)
    return false;
  *series = (uint8_t)ser;
  *parallel = (uint8_t)par;
  return true;
}

bool config_parse(const char *args, battery_config_t *out,
                  const char **err_field) {
  char buf[192];
  strlcpy(buf, args, sizeof(buf));

  char *f[8];
  int n = split(buf, f, 8);
  if (n < 6 || n > 7) {
    *err_field = "expected 6 or 7 fields";
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->version = CFG_VERSION;

  if (!chem_from_name(f[0], &out->chem)) {
    *err_field = "chem (LifePo|LiIon|AGM|Acid)";
    return false;
  }
  if (!parse_sp(f[1], &out->series, &out->parallel)) {
    *err_field = "xSyP";
    return false;
  }

  char *end = NULL;
  long mah = strtol(f[2], &end, 10);
  if (*end || mah < 1 || mah > 1000000) {
    *err_field = "mAh (1..1000000)";
    return false;
  }
  out->capacity_mah = (uint32_t)mah;

  out->vmin = strtof(f[3], &end);
  if (*end || out->vmin < 0.1f || out->vmin > 100.0f) {
    *err_field = "Vmin (0.1..100)";
    return false;
  }
  out->vmax = strtof(f[4], &end);
  if (*end || out->vmax <= out->vmin || out->vmax > 100.0f) {
    *err_field = "Vmax (>Vmin, <=100)";
    return false;
  }
  out->imax = strtof(f[5], &end);
  if (*end || out->imax < 0.01f || out->imax > 10.0f) {
    *err_field = "Imax (0.01..10)";
    return false;
  }

  // pack_id is optional. Multiple interchangeable packs each keep their own
  // stored count and learned capacity; omitting it means a single pack.
  if (n == 7) {
    for (const char *p = f[6]; *p; p++) {
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
        *err_field = "pack_id (alphanumeric, - and _ only)";
        return false;
      }
    }
    if (strlen(f[6]) == 0 || strlen(f[6]) >= CFG_PACK_ID_MAX) {
      *err_field = "pack_id (1..15 chars)";
      return false;
    }
    strlcpy(out->pack_id, f[6], sizeof(out->pack_id));
  } else {
    strlcpy(out->pack_id, "default", sizeof(out->pack_id));
  }

  return true;
}

void config_format(const battery_config_t *cfg, char *buf, size_t len) {
  snprintf(buf, len, "%s,%uS%uP,%lu,%.2f,%.2f,%.2f,%s", chem_name(cfg->chem),
           cfg->series, cfg->parallel, (unsigned long)cfg->capacity_mah,
           cfg->vmin, cfg->vmax, cfg->imax, cfg->pack_id);
}

void config_warn_if_odd(const battery_config_t *cfg) {
  float per_cell = cfg->vmax / cfg->series;
  if (per_cell < CHEM_VMAX_LO[cfg->chem] || per_cell > CHEM_VMAX_HI[cfg->chem])
    ESP_LOGW(TAG,
             "Vmax %.2f V / %uS = %.3f V/cell, outside the usual %.2f-%.2f for "
             "%s. Note Vmax is the CHARGE ceiling, not resting-full.",
             cfg->vmax, cfg->series, per_cell, CHEM_VMAX_LO[cfg->chem],
             CHEM_VMAX_HI[cfg->chem], chem_name(cfg->chem));

  float per_cell_min = cfg->vmin / cfg->series;
  if (per_cell_min < 1.5f)
    ESP_LOGW(TAG, "Vmin %.2f V / %uS = %.3f V/cell looks low for %s",
             cfg->vmin, cfg->series, per_cell_min, chem_name(cfg->chem));
}
