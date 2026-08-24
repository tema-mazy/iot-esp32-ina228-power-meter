// Battery provisioning. Nothing about the battery is compiled in: chemistry,
// cell count, capacity and limits all arrive at runtime via ATS and live in
// NVS. See DEVELOPMENT_PLAN.md S2.2.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_PACK_ID_MAX 16
#define CFG_VERSION     1

typedef enum {
  CHEM_LIFEPO = 0,
  CHEM_LIION,
  CHEM_AGM,
  CHEM_ACID,
  CHEM_COUNT,
} chem_t;

typedef struct {
  uint16_t version;      // CFG_VERSION; guards NVS blobs across upgrades
  chem_t   chem;
  uint8_t  series;       // cells in series
  uint8_t  parallel;     // strings in parallel
  uint32_t capacity_mah;
  float    vmin;         // pack level, discharge floor
  float    vmax;         // pack level, CHARGE CEILING - not the resting-full
                         // voltage. For lead-acid these differ by ~1.8 V; the
                         // resting figure comes from the OCV table (plan S4).
  float    imax;         // sets CURRENT_LSB and ADCRANGE
  float    i_offset_a;   // zero-current calibration, applied by the gauge.
                         // Present from v1 so adding it needs no NVS version
                         // bump; written by the offset calibration in phase 5.
  char     pack_id[CFG_PACK_ID_MAX];
} battery_config_t;

const char *chem_name(chem_t c);
bool        chem_from_name(const char *s, chem_t *out);

// Parses the argument tail of ATS=. On failure returns false and points
// err_field at a static string naming the offending field.
bool config_parse(const char *args, battery_config_t *out, const char **err_field);

// Renders in the same field order ATS accepts, so ATS? round-trips.
void config_format(const battery_config_t *cfg, char *buf, size_t len);

// Chemistry sanity check on Vmax/series. Warns rather than rejects: unusual
// packs exist, and refusing to gauge one is worse than noting it.
void config_warn_if_odd(const battery_config_t *cfg);
