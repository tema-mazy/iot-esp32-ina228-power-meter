// Fuel gauge. Coulomb counting from the INA228's hardware accumulator, with
// open-circuit-voltage seeding at power-up. See DEVELOPMENT_PLAN.md S4.
#pragma once

#include "config.h"
#include "gauge_fwd.h"
#include "ina228.h"
#include <stdbool.h>
#include <stdint.h>

// 2: added v_full_pc. A version or size mismatch makes storage_load_gauge
// reject the blob, so the gauge simply re-seeds from OCV once after an
// upgrade. Nothing needs migrating.
#define GAUGE_VERSION 2

// Currents below this are not accumulated. The measured zero offset is about
// -0.13 mA (-2.1 uV at the shunt), which is roughly twice the INA228's spec'd
// Vos and therefore most likely thermoelectric EMF at the shunt joints. That
// drifts with temperature, so subtracting a stored calibration is not enough
// on its own: without a deadband the gauge invents about -0.8 %/month of
// discharge while idle, always in the same direction.
#define GAUGE_DEADBAND_A 0.0005f

// Charge termination: current has tapered below C/50 with voltage at the
// ceiling. Both conditions are required - voltage alone is meaningless while
// a charger is driving the pack.
#define GAUGE_ANCHOR_C_DIV   50
#define GAUGE_ANCHOR_HOLD_S  60

// Above this the load is real; below it the pack is resting and voltage can
// be trusted as OCV.
#define GAUGE_REST_C_DIV     100

// Restoring a stored count needs the voltage to be roughly where we left it.
// Wider than measurement noise, narrower than a meaningful SoC change.
#define GAUGE_SAME_PACK_MV_PER_CELL 50.0f

// Assume-full (ATR, button long press) is refused below this table SoC. The
// point is to reject "assume full" on a visibly half-empty pack, not to insist
// on one particular chemistry's charge ceiling: tool packs commonly terminate
// near 4.10 V/cell rather than 4.20, and hardcoding the table top rejected
// them. Expressed in table percent so it holds for every chemistry.
#define GAUGE_ASSUME_FULL_MIN_SOC 80.0f

// How long the pack must sit at rest before a reading counts as settled
// enough to learn the full voltage from. Nobody in the field sends ATR, so
// v_full has to arrive on its own or a freshly charged tool pack reads ~92 %
// forever.
//
// The number this defends against is surface charge: straight off the charger
// a Li-ion cell reads 4.15-4.18 V and decays toward its true ~4.10 over tens
// of minutes. Learning from that would set v_full high and make SoC read low
// for the pack's whole life - the same bug in the other direction. 20 minutes
// covers most of the decay while still being a rest a garage pack actually
// gets between jobs.
#define GAUGE_VFULL_SETTLE_S 1200.0f

typedef enum {
  GS_UNKNOWN = 0,
  GS_CHARGING,
  GS_DISCHARGING,
  GS_IDLE,
  GS_FULL,
} gauge_mode_t;

// Persisted per pack_id. The INA228's CHARGE register is volatile and dies
// with power, so this is the only durable record of accumulated charge.
typedef struct gauge_persist_s {
  uint16_t version;
  float    mah_remaining;
  float    mah_full;      // learned capacity, seeded from ATS
  float    last_v;        // pack voltage at last save, for same-pack checks
  float    r_total;       // learned internal resistance, ohms
  float    v_full_pc;     // learned resting-full volts per cell, 0 = unknown.
                          // NOT config's vmax: that is the charge ceiling,
                          // which for lead-acid sits ~1.8 V above the resting
                          // figure. Learned instead from whatever voltage the
                          // pack actually settles at when declared full.
  float    i_offset_a;    // zero-current calibration
  uint32_t full_charges;  // times the anchor has fired
  bool     est;           // SoC came from OCV in a flat region
} gauge_persist_t;

typedef struct {
  float        soc;         // percent
  float        mah_left;
  float        mah_used;    // since the last full
  float        wh_left;
  gauge_mode_t mode;
  bool         est;
  float        v_ocv;       // IR-compensated, or measured when resting
  float        r_total;
  float        v_full_pc;   // learned resting-full V/cell, 0 = not yet seen
} gauge_info_t;

// Loads persisted state for cfg->pack_id and decides whether to restore the
// stored count or re-seed from voltage.
void gauge_init(const battery_config_t *cfg, const ina228_reading_t *first);

// Call once per poll. dt_s is the interval since the previous call.
void gauge_update(const battery_config_t *cfg, const ina228_reading_t *r,
                  float dt_s);

bool gauge_get(gauge_info_t *out);

// ATR / button long press: declare a swap and assume the pack is full.
// Refused when resting voltage says otherwise (returns false).
bool gauge_reseed_full(const battery_config_t *cfg, const ina228_reading_t *r);

// ATC: force remaining capacity outright.
void gauge_set_remaining(const battery_config_t *cfg, float mah);

const char *gauge_mode_name(gauge_mode_t m);

// SoC from resting volts per cell for a chemistry, with a flag for the
// stretches where the curve is too flat to trust.
//
// v_full_pc rescales the result so that voltage reads 100 %; pass 0 to use the
// table's own top. A pack whose charger terminates at 4.10 V/cell holds less
// charge than one taken to 4.20, but its owner still calls it full, and the
// charge below that point is unchanged - so the whole curve scales by the same
// factor rather than shifting. See DEVELOPMENT_PLAN.md S4.
float gauge_soc_from_ocv(chem_t chem, float v_per_cell, float v_full_pc,
                         bool *est_out);
