// INA228 driver - 20-bit I2C power/energy monitor with hardware charge
// accumulation. All register widths and LSB values below are verified against
// TI datasheet SLYS021A (Jan 2021, rev May 2022).
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// -- Register map (SLYS021A S7.6.1) -------------------------------------------
#define INA228_REG_CONFIG          0x00 // 16b: RST b15, RSTACC b14, ADCRANGE b4
#define INA228_REG_ADC_CONFIG      0x01 // 16b: MODE, VBUSCT, VSHCT, VTCT, AVG
#define INA228_REG_SHUNT_CAL       0x02 // 15b (b14-0); b15 reserved
#define INA228_REG_SHUNT_TEMPCO    0x03 // 14b
#define INA228_REG_VSHUNT          0x04 // 24b, value in b23-4, signed
#define INA228_REG_VBUS            0x05 // 24b, value in b23-4, always positive
#define INA228_REG_DIETEMP         0x06 // 16b, signed, NOT shifted
#define INA228_REG_CURRENT         0x07 // 24b, value in b23-4, signed
#define INA228_REG_POWER           0x08 // 24b, b23-0 unsigned, NOT shifted
#define INA228_REG_ENERGY          0x09 // 40b unsigned
#define INA228_REG_CHARGE          0x0A // 40b signed
#define INA228_REG_DIAG_ALRT       0x0B // 16b
#define INA228_REG_MANUFACTURER_ID 0x3E // 16b: 0x5449 ("TI")
#define INA228_REG_DEVICE_ID       0x3F // 16b: DIEID b15-4 = 0x228, REV b3-0

#define INA228_MANUFACTURER_ID 0x5449
#define INA228_DEVICE_ID       0x228 // upper 12 bits of DEVICE_ID

// -- CONFIG bits --------------------------------------------------------------
#define INA228_CFG_RST      (1U << 15)
#define INA228_CFG_RSTACC   (1U << 14)
#define INA228_CFG_ADCRANGE (1U << 4)

// -- DIAG_ALRT bits (S7.6.1.12) -----------------------------------------------
#define INA228_DIAG_ENERGYOF (1U << 11) // ENERGY overflowed; clears on read
#define INA228_DIAG_CHARGEOF (1U << 10) // CHARGE overflowed; clears on read
#define INA228_DIAG_MATHOF   (1U << 9)  // arithmetic overflow; needs RSTACC
#define INA228_DIAG_CNVRF    (1U << 1)  // conversion complete
#define INA228_DIAG_MEMSTAT  (1U << 0)  // 0 = trim memory checksum error

// -- Fixed conversion constants (S8.1.1, Table 8-1) ---------------------------
#define INA228_VBUS_LSB_UV       195.3125  // uV per LSB
#define INA228_VSHUNT_LSB_NV_R0  312.5     // nV per LSB, ADCRANGE = 0
#define INA228_VSHUNT_LSB_NV_R1  78.125    // nV per LSB, ADCRANGE = 1
#define INA228_DIETEMP_LSB_MC    7.8125    // mdegC per LSB
#define INA228_SHUNT_CAL_CONST   13107.2e6 // S8.1.2 eq.2
#define INA228_SHUNT_CAL_MAX     0x7FFF    // 15-bit field

// ADCRANGE = 1 gives 4x resolution but only +/-40.96 mV across the shunt.
// Below this current it is worth taking; above it we need the wide range.
#define INA228_ADCRANGE1_MAX_MV 40.96

typedef struct {
  float    bus_v;     // V
  float    shunt_v;   // V across the shunt
  float    current_a; // A, positive = discharge (out of the battery)
  float    power_w;   // W
  float    temp_c;    // degC, die temperature
  double   charge_c;  // C, hardware-accumulated since last RSTACC
  double   energy_j;  // J, hardware-accumulated
  uint16_t diag;      // raw DIAG_ALRT
} ina228_reading_t;

typedef struct {
  int      sda_gpio;
  int      scl_gpio;
  uint8_t  addr;         // 0x40 with A0/A1 open
  uint32_t freq_hz;      // 400000; drop to 100000 for cables > ~20 cm
  uint32_t rshunt_uohm;  // 15000 for the Adafruit 5832 onboard shunt
  float    imax_a;       // max expected current; sets CURRENT_LSB
} ina228_config_t;

// Probes MANUFACTURER_ID/DEVICE_ID, resets accumulators, configures the ADC
// for continuous bus+shunt+temperature, and applies the calibration.
esp_err_t ina228_init(const ina228_config_t *cfg);

// Recomputes CURRENT_LSB, picks ADCRANGE and writes SHUNT_CAL. Called by
// ina228_init, and again whenever ATS changes Imax (plan S2.2).
esp_err_t ina228_set_calibration(uint32_t rshunt_uohm, float imax_a);

esp_err_t ina228_read(ina228_reading_t *out);

// Zeroes the hardware ENERGY and CHARGE accumulators. The registers are
// volatile and die with power, so NVS is the source of truth (plan S4).
esp_err_t ina228_reset_accumulators(void);

float ina228_current_lsb(void); // A per LSB, for diagnostics
