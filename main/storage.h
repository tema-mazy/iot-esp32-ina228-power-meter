// NVS persistence for battery provisioning.
//
// Each pack_id keeps its own record, so swapping between known packs restores
// that pack's configuration rather than treating every swap as a new battery
// (handoff S7.4). A separate key holds which pack_id is active.
#pragma once

#include "config.h"
#include "gauge_fwd.h"
#include "esp_err.h"

esp_err_t storage_init(void);

// Saves under cfg->pack_id and makes it the active pack.
esp_err_t storage_save_config(const battery_config_t *cfg);

// Loads the active pack's config. ESP_ERR_NVS_NOT_FOUND if unprovisioned.
esp_err_t storage_load_config(battery_config_t *out);

// Loads a specific pack by id without making it active.
esp_err_t storage_load_pack(const char *pack_id, battery_config_t *out);

// Gauge state, stored alongside the config under the same pack_id. Separate
// key because it is rewritten every 30 s while the config almost never
// changes -- mixing them would multiply NVS wear on the config.
esp_err_t storage_save_gauge(const char *pack_id, const gauge_persist_t *st);
esp_err_t storage_load_gauge(const char *pack_id, gauge_persist_t *out);
