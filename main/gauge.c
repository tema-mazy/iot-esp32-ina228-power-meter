#include "gauge.h"

#include "esp_log.h"
#include "storage.h"
#include <math.h>
#include <string.h>

static const char *TAG = "gauge";

static gauge_persist_t s_st;
static bool            s_loaded;
static double          s_last_charge_c;  // previous CHARGE register value
static bool            s_have_charge;
static gauge_mode_t    s_mode;
static float           s_anchor_held_s;
static float           s_since_save_s;
static float           s_mah_at_save;
static float           s_v_ocv;

// R learning state
#define R_RING 9
#define R_LEARN_MIN_DI_A 0.030f   // below this the dV is mostly noise
#define R_MIN_OHM        0.001f
#define R_MAX_OHM        10.0f
static float s_r_ring[R_RING];
static int   s_r_n;
static float s_prev_v, s_prev_i;
static bool  s_have_prev;
static bool  s_need_seed;

// ---- OCV tables -------------------------------------------------------------
// Volts per cell -> percent, descending. Chemistry is selected at runtime from
// ATS, so nothing here is compiled in for a particular battery.

typedef struct { float v; float soc; } ocv_pt_t;

// Li-ion NMC: slopes usably end to end, +/-5-10 %.
static const ocv_pt_t OCV_LIION[] = {
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 70}, {3.82f, 60},
    {3.72f, 50},  {3.65f, 40}, {3.58f, 30}, {3.50f, 20}, {3.40f, 10},
    {3.20f, 5},   {3.00f, 0},
};
// LiFePO4: informative only at the two knees, the middle is nearly flat.
static const ocv_pt_t OCV_LIFEPO[] = {
    {3.45f, 100}, {3.35f, 90}, {3.30f, 70}, {3.28f, 50},
    {3.25f, 30},  {3.20f, 20}, {3.00f, 10}, {2.50f, 0},
};
// Lead-acid, per cell. Needs hours of rest to settle, not the 60 s that
// suffices for lithium; surface charge alone reads ~0.1 V/cell high.
static const ocv_pt_t OCV_AGM[] = {
    {2.14f, 100}, {2.09f, 75}, {2.04f, 50}, {1.99f, 25}, {1.93f, 0},
};
static const ocv_pt_t OCV_ACID[] = {
    {2.12f, 100}, {2.07f, 75}, {2.03f, 50}, {1.98f, 25}, {1.95f, 0},
};

// Bands where the curve is too flat for the reading to mean much.
static const float FLAT_LO[CHEM_COUNT] = {3.20f, 3.60f, 0.0f, 0.0f};
static const float FLAT_HI[CHEM_COUNT] = {3.35f, 3.90f, 0.0f, 0.0f};

static const ocv_pt_t *ocv_table(chem_t c, int *n) {
  switch (c) {
  case CHEM_LIION:  *n = sizeof(OCV_LIION) / sizeof(ocv_pt_t);  return OCV_LIION;
  case CHEM_LIFEPO: *n = sizeof(OCV_LIFEPO) / sizeof(ocv_pt_t); return OCV_LIFEPO;
  case CHEM_AGM:    *n = sizeof(OCV_AGM) / sizeof(ocv_pt_t);    return OCV_AGM;
  default:          *n = sizeof(OCV_ACID) / sizeof(ocv_pt_t);   return OCV_ACID;
  }
}

float gauge_soc_from_ocv(chem_t chem, float v, bool *est_out) {
  int n;
  const ocv_pt_t *t = ocv_table(chem, &n);

  if (est_out)
    *est_out = (v >= FLAT_LO[chem] && v <= FLAT_HI[chem]);

  if (v >= t[0].v)      return 100.0f;
  if (v <= t[n - 1].v)  return 0.0f;

  for (int i = 0; i < n - 1; i++) {
    if (v <= t[i].v && v >= t[i + 1].v) {
      float span = t[i].v - t[i + 1].v;
      float f = span > 0 ? (v - t[i + 1].v) / span : 0;
      return t[i + 1].soc + f * (t[i].soc - t[i + 1].soc);
    }
  }
  return 0.0f;
}

const char *gauge_mode_name(gauge_mode_t m) {
  switch (m) {
  case GS_CHARGING:    return "charging";
  case GS_DISCHARGING: return "discharging";
  case GS_IDLE:        return "idle";
  case GS_FULL:        return "full";
  default:             return "unknown";
  }
}

// ---- seeding ----------------------------------------------------------------

static void seed_from_ocv(const battery_config_t *cfg, float v_pack,
                          const char *why) {
  bool est = false;
  float per_cell = v_pack / cfg->series;
  float soc = gauge_soc_from_ocv(cfg->chem, per_cell, &est);

  // Above the table's 100 % point the reading is not a rested voltage at all:
  // a charger is attached, or surface charge has not decayed. Seeding from it
  // would report full on a half-empty battery.
  int n;
  const ocv_pt_t *t = ocv_table(cfg->chem, &n);
  if (per_cell > t[0].v * 1.02f) {
    ESP_LOGW(TAG, "%.3f V/cell is above the table top - not a rested voltage, "
                  "holding SoC and flagging est", per_cell);
    est = true;
    soc = s_st.mah_full > 0 ? 100.0f * s_st.mah_remaining / s_st.mah_full : 50.0f;
  }

  s_st.mah_remaining = soc / 100.0f * s_st.mah_full;
  s_st.est = est;
  ESP_LOGI(TAG, "seeded %.1f%% (%.0f mAh) from %.3f V/cell [%s]%s", soc,
           s_st.mah_remaining, per_cell, why, est ? " est" : "");
}

void gauge_init(const battery_config_t *cfg, const ina228_reading_t *first) {
  memset(&s_st, 0, sizeof(s_st));
  s_st.version  = GAUGE_VERSION;
  s_st.mah_full = (float)cfg->capacity_mah;
  s_st.r_total  = 0.0f;

  gauge_persist_t stored;
  bool have = (storage_load_gauge(cfg->pack_id, &stored) == ESP_OK &&
               stored.version == GAUGE_VERSION);
  if (have) {
    s_st = stored;
    if (s_st.mah_full <= 0)
      s_st.mah_full = (float)cfg->capacity_mah;
  }

  float v = first ? first->bus_v : 0.0f;
  if (v <= 0.5f) {
    // Seeding from a zero reading would report an empty battery on a full
    // pack. Wait for a real measurement instead.
    ESP_LOGW(TAG, "no valid voltage at init (%.3f V), deferring seed", v);
    s_need_seed = true;
    s_loaded = true;
    return;
  }

  if (!have) {
    seed_from_ocv(cfg, v, "no stored state");
  } else {
    // The monitor is powered by the battery, so a power-on reset means the
    // pack was disconnected. If it comes back at the voltage we left it at,
    // it is the same pack undisturbed and the stored count beats OCV.
    float delta_per_cell = fabsf(v - s_st.last_v) / cfg->series * 1000.0f;
    if (delta_per_cell <= GAUGE_SAME_PACK_MV_PER_CELL) {
      ESP_LOGI(TAG, "restored %.0f mAh (%.1f%%), voltage moved %.0f mV/cell",
               s_st.mah_remaining, 100.0f * s_st.mah_remaining / s_st.mah_full,
               delta_per_cell);
    } else {
      seed_from_ocv(cfg, v, "voltage moved while off");
    }
  }

  s_mah_at_save = s_st.mah_remaining;
  s_loaded = true;
  s_have_charge = false;
  s_have_prev = false;
  s_r_n = 0;
}

// ---- update -----------------------------------------------------------------

static void save(const battery_config_t *cfg, float v) {
  s_st.last_v = v;
  if (storage_save_gauge(cfg->pack_id, &s_st) == ESP_OK) {
    s_mah_at_save  = s_st.mah_remaining;
    s_since_save_s = 0;
  }
}

void gauge_update(const battery_config_t *cfg, const ina228_reading_t *r,
                  float dt_s) {
  if (!s_loaded || dt_s <= 0)
    return;

  // First reading after init establishes the baseline; the CHARGE register is
  // an absolute accumulator, so only differences are meaningful.
  if (!s_have_charge) {
    s_last_charge_c = r->charge_c;
    s_have_charge = true;
    return;
  }

  // A deferred seed waits for a plausible voltage rather than taking the next
  // reading regardless -- the next one can still be zero.
  if (s_need_seed && r->bus_v > 0.5f) {
    seed_from_ocv(cfg, r->bus_v, "deferred seed");
    s_need_seed = false;
    save(cfg, r->bus_v);
  }

  double delta_c = r->charge_c - s_last_charge_c;
  s_last_charge_c = r->charge_c;

  // Correct the fixed offset, then deadband the residual. Both are needed:
  // the calibration removes the bulk, the deadband survives its drift.
  double corrected_c = delta_c - (double)s_st.i_offset_a * dt_s;
  float avg_i = (float)(corrected_c / dt_s);
  if (fabsf(avg_i) < GAUGE_DEADBAND_A)
    corrected_c = 0;

  // Positive current is discharge, so charge leaving the pack reduces the
  // count. mAh = coulombs / 3.6.
  s_st.mah_remaining -= (float)(corrected_c / 3.6);
  if (s_st.mah_remaining < 0)
    s_st.mah_remaining = 0;
  if (s_st.mah_remaining > s_st.mah_full)
    s_st.mah_remaining = s_st.mah_full;

  // ---- mode ----
  float rest_thresh = s_st.mah_full / 1000.0f / GAUGE_REST_C_DIV; // amps
  if (rest_thresh < 0.002f)
    rest_thresh = 0.002f;
  if (r->current_a < -rest_thresh)
    s_mode = GS_CHARGING;
  else if (r->current_a > rest_thresh)
    s_mode = GS_DISCHARGING;
  else
    s_mode = GS_IDLE;

  // ---- learn R_total from load steps ----
  // Every step in current gives R = -dV/dI for free. Measured on real packs
  // this varies 20x between batteries of the same nominal voltage (0.92 ohm
  // on a 5S1P 18650 versus 40-70 mOhm expected of a 5S3P 21700), so it must
  // be learned rather than assumed. A median over recent estimates rejects
  // the outliers that a single noisy step would produce.
  if (s_have_prev) {
    float di = r->current_a - s_prev_i;
    if (fabsf(di) > R_LEARN_MIN_DI_A) {
      float est = -(r->bus_v - s_prev_v) / di;
      if (est > R_MIN_OHM && est < R_MAX_OHM) {
        s_r_ring[s_r_n % R_RING] = est;
        s_r_n++;
        int n = s_r_n < R_RING ? s_r_n : R_RING;
        float sorted[R_RING];
        memcpy(sorted, s_r_ring, sizeof(float) * n);
        for (int i = 1; i < n; i++) {          // insertion sort, n <= 9
          float v = sorted[i];
          int j = i - 1;
          while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
          sorted[j + 1] = v;
        }
        float med = sorted[n / 2];
        if (n >= 3)
          s_st.r_total = med;
      }
    }
  }
  s_prev_v = r->bus_v;
  s_prev_i = r->current_a;
  s_have_prev = true;

  // ---- IR compensation ----
  // v_ocv = v_measured + I * R. Zero until R has been learned, in which case
  // this reports the measured voltage unmodified.
  s_v_ocv = r->bus_v + r->current_a * s_st.r_total;

  // ---- full-charge anchor ----
  float taper = s_st.mah_full / 1000.0f / GAUGE_ANCHOR_C_DIV;
  if (r->bus_v >= cfg->vmax - 0.1f && fabsf(r->current_a) < taper) {
    s_anchor_held_s += dt_s;
    if (s_anchor_held_s >= GAUGE_ANCHOR_HOLD_S) {
      float before = s_st.mah_remaining;
      if (fabsf(before - s_st.mah_full) / s_st.mah_full > 0.02f) {
        // The count disagreed with reality by more than 2 %. Trust the anchor
        // and treat the difference as capacity learning.
        ESP_LOGW(TAG, "anchor: count was %.0f mAh, full is %.0f - relearning",
                 before, s_st.mah_full);
      }
      s_st.mah_remaining = s_st.mah_full;
      s_st.est = false;
      s_st.full_charges++;
      s_mode = GS_FULL;
      s_anchor_held_s = 0;
      save(cfg, r->bus_v);
    }
  } else {
    s_anchor_held_s = 0;
  }

  // ---- empty clamp ----
  if (r->bus_v <= cfg->vmin && s_st.mah_remaining > 0) {
    ESP_LOGW(TAG, "at Vmin with %.0f mAh still counted - clamping to 0",
             s_st.mah_remaining);
    s_st.mah_remaining = 0;
    s_st.est = false;
    save(cfg, r->bus_v);
  }

  // ---- persistence ----
  // Power is cut abruptly whenever the pack is disconnected, so this is doing
  // real work rather than guarding against rare brownouts.
  s_since_save_s += dt_s;
  if (s_since_save_s >= 30.0f ||
      fabsf(s_st.mah_remaining - s_mah_at_save) >= 10.0f)
    save(cfg, r->bus_v);
}

bool gauge_get(gauge_info_t *out) {
  if (!s_loaded || s_st.mah_full <= 0)
    return false;
  out->soc      = 100.0f * s_st.mah_remaining / s_st.mah_full;
  out->mah_left = s_st.mah_remaining;
  out->mah_used = s_st.mah_full - s_st.mah_remaining;
  out->wh_left  = s_st.mah_remaining / 1000.0f * s_v_ocv;
  out->mode     = s_mode;
  out->est      = s_st.est;
  out->v_ocv    = s_v_ocv;
  out->r_total  = s_st.r_total;
  return true;
}

bool gauge_reseed_full(const battery_config_t *cfg,
                       const ina228_reading_t *r) {
  // Refuse when the pack visibly is not full. "Assume full" on a half-empty
  // battery produces a confidently wrong gauge, which is worse than one that
  // admits it does not know.
  float per_cell = (r->bus_v + r->current_a * s_st.r_total) / cfg->series;
  int n;
  const ocv_pt_t *t = ocv_table(cfg->chem, &n);
  float full_thresh = t[0].v - (t[0].v - t[1].v) * 0.8f;

  if (per_cell < full_thresh) {
    ESP_LOGW(TAG, "refusing assume-full: %.3f V/cell is below %.3f",
             per_cell, full_thresh);
    return false;
  }

  s_st.mah_remaining = s_st.mah_full;
  s_st.est = false;
  save(cfg, r->bus_v);
  ESP_LOGI(TAG, "reseeded to full (%.0f mAh)", s_st.mah_full);
  return true;
}

void gauge_set_remaining(const battery_config_t *cfg, float mah) {
  if (mah < 0)
    mah = 0;
  if (mah > s_st.mah_full)
    mah = s_st.mah_full;
  s_st.mah_remaining = mah;
  s_st.est = false;
  save(cfg, s_st.last_v);
  ESP_LOGI(TAG, "remaining forced to %.0f mAh", mah);
}
