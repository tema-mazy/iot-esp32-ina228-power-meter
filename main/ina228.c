#include "ina228.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ina228";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static float                   s_current_lsb;  // A per LSB
static bool                    s_adcrange1;    // true = +/-40.96 mV range

#define I2C_TIMEOUT_MS 100

// -- Register access ----------------------------------------------------------
// All registers are big-endian, addressed by a single pointer byte.

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                     pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t reg_write16(uint8_t reg, uint16_t val) {
  uint8_t tx[3] = {reg, (uint8_t)(val >> 8), (uint8_t)val};
  return i2c_master_transmit(s_dev, tx, sizeof(tx), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t reg_read16(uint8_t reg, uint16_t *val) {
  uint8_t b[2];
  esp_err_t err = reg_read(reg, b, 2);
  if (err == ESP_OK)
    *val = ((uint16_t)b[0] << 8) | b[1];
  return err;
}

// VSHUNT, VBUS and CURRENT hold their value in bits 23-4; bits 3-0 read 0.
// Shift right by 4 to get the 20-bit result, then sign-extend from bit 19.
static esp_err_t reg_read20_signed(uint8_t reg, int32_t *val) {
  uint8_t b[3];
  esp_err_t err = reg_read(reg, b, 3);
  if (err != ESP_OK)
    return err;
  int32_t raw = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2];
  raw >>= 4;
  if (raw & 0x80000)
    raw -= 0x100000;
  *val = raw;
  return ESP_OK;
}

// POWER is 24-bit unsigned occupying the whole register - no shift.
static esp_err_t reg_read24_unsigned(uint8_t reg, uint32_t *val) {
  uint8_t b[3];
  esp_err_t err = reg_read(reg, b, 3);
  if (err != ESP_OK)
    return err;
  *val = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
  return ESP_OK;
}

static esp_err_t reg_read40(uint8_t reg, int64_t *val, bool is_signed) {
  uint8_t b[5];
  esp_err_t err = reg_read(reg, b, 5);
  if (err != ESP_OK)
    return err;
  int64_t raw = 0;
  for (int i = 0; i < 5; i++)
    raw = (raw << 8) | b[i];
  if (is_signed && (raw & ((int64_t)1 << 39)))
    raw -= ((int64_t)1 << 40);
  *val = raw;
  return ESP_OK;
}

// -- Calibration (SLYS021A S8.1.2) --------------------------------------------
//   CURRENT_LSB = Imax / 2^19
//   SHUNT_CAL   = 13107.2e6 * CURRENT_LSB * Rshunt      (x4 when ADCRANGE = 1)

esp_err_t ina228_set_calibration(uint32_t rshunt_uohm, float imax_a) {
  if (imax_a <= 0.0f || rshunt_uohm == 0)
    return ESP_ERR_INVALID_ARG;

  const float rshunt = rshunt_uohm / 1e6f;
  s_current_lsb = imax_a / 524288.0f; // 2^19

  // ADCRANGE = 1 is 4x finer but saturates at +/-40.96 mV across the shunt.
  // Take it whenever the full expected current still fits.
  const float full_scale_mv = imax_a * rshunt * 1000.0f;
  s_adcrange1 = (full_scale_mv <= INA228_ADCRANGE1_MAX_MV);

  double cal = INA228_SHUNT_CAL_CONST * (double)s_current_lsb * (double)rshunt;
  if (s_adcrange1)
    cal *= 4.0;

  if (cal > INA228_SHUNT_CAL_MAX) {
    // SHUNT_CAL is a 15-bit field. Overflowing it means Imax x Rshunt is too
    // large for this part; the current reading would silently be wrong.
    ESP_LOGE(TAG, "SHUNT_CAL %.0f exceeds 15-bit max %d (Imax=%.2fA R=%luuOhm)",
             cal, INA228_SHUNT_CAL_MAX, imax_a, (unsigned long)rshunt_uohm);
    return ESP_ERR_INVALID_ARG;
  }

  uint16_t cfg = 0;
  esp_err_t err = reg_read16(INA228_REG_CONFIG, &cfg);
  if (err != ESP_OK)
    return err;
  cfg = s_adcrange1 ? (cfg | INA228_CFG_ADCRANGE) : (cfg & ~INA228_CFG_ADCRANGE);
  err = reg_write16(INA228_REG_CONFIG, cfg);
  if (err != ESP_OK)
    return err;

  err = reg_write16(INA228_REG_SHUNT_CAL, (uint16_t)(cal + 0.5));
  if (err != ESP_OK)
    return err;

  ESP_LOGI(TAG,
           "cal: Imax=%.2fA Rshunt=%lu uOhm -> CURRENT_LSB=%.4f uA, "
           "SHUNT_CAL=%u, ADCRANGE=%d (%.2f mV full scale)",
           imax_a, (unsigned long)rshunt_uohm, s_current_lsb * 1e6f,
           (unsigned)(cal + 0.5), s_adcrange1 ? 1 : 0, full_scale_mv);
  return ESP_OK;
}

float ina228_current_lsb(void) { return s_current_lsb; }

esp_err_t ina228_reset_accumulators(void) {
  uint16_t cfg = 0;
  esp_err_t err = reg_read16(INA228_REG_CONFIG, &cfg);
  if (err != ESP_OK)
    return err;
  return reg_write16(INA228_REG_CONFIG, cfg | INA228_CFG_RSTACC);
}

// -- Init ---------------------------------------------------------------------

esp_err_t ina228_init(const ina228_config_t *cfg) {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port                     = I2C_NUM_0,
      .sda_io_num                   = cfg->sda_gpio,
      .scl_io_num                   = cfg->scl_gpio,
      .clk_source                   = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt            = 7,
      // The Adafruit breakout has 4.7k pull-ups; internal ones are far weaker
      // and would only slow the edges. Leave them off.
      .flags.enable_internal_pullup = false,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address  = cfg->addr,
      .scl_speed_hz    = cfg->freq_hz,
  };
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG,
                      "i2c dev");

  // Probe identity before trusting anything else. A wrong or absent chip here
  // makes every downstream reading meaningless (plan S3.4).
  uint16_t mfg = 0, dev = 0;
  ESP_RETURN_ON_ERROR(reg_read16(INA228_REG_MANUFACTURER_ID, &mfg), TAG,
                      "read MANUFACTURER_ID (no ACK at 0x%02x?)", cfg->addr);
  ESP_RETURN_ON_ERROR(reg_read16(INA228_REG_DEVICE_ID, &dev), TAG,
                      "read DEVICE_ID");

  if (mfg != INA228_MANUFACTURER_ID || (dev >> 4) != INA228_DEVICE_ID) {
    ESP_LOGE(TAG, "wrong device: MFG=0x%04x (want 0x%04x) DEV=0x%04x (want 0x228x)",
             mfg, INA228_MANUFACTURER_ID, dev);
    return ESP_ERR_NOT_SUPPORTED;
  }
  ESP_LOGI(TAG, "found INA228 at 0x%02x, die 0x%03x rev %u", cfg->addr,
           dev >> 4, dev & 0x0F);

  // Continuous bus + shunt + temperature, 1052 us conversions, 64x averaging.
  // MODE=Fh VBUSCT=5h VSHCT=5h VTCT=5h AVG=3h -> ~202 ms per averaged sample.
  // Averaging happens in the chip, so the 1 Hz poll rate does not affect the
  // hardware charge accumulation at all (plan S3.3).
  ESP_RETURN_ON_ERROR(reg_write16(INA228_REG_ADC_CONFIG, 0xFB6B), TAG, "adc cfg");

  ESP_RETURN_ON_ERROR(ina228_set_calibration(cfg->rshunt_uohm, cfg->imax_a), TAG,
                      "calibration");
  ESP_RETURN_ON_ERROR(ina228_reset_accumulators(), TAG, "rstacc");

  // Wait for the first conversion before returning. With AVG=64 at 1052 us a
  // full set takes ~202 ms, and every register reads zero until then. A caller
  // that reads immediately gets 0 V and 0 A, which looks exactly like a flat
  // battery -- the fuel gauge seeded 0 % on a full pack because of this.
  for (int i = 0; i < 20; i++) {
    vTaskDelay(pdMS_TO_TICKS(25));
    uint16_t diag = 0;
    if (reg_read16(INA228_REG_DIAG_ALRT, &diag) == ESP_OK &&
        (diag & INA228_DIAG_CNVRF)) {
      ESP_LOGI(TAG, "first conversion ready after %d ms", (i + 1) * 25);
      return ESP_OK;
    }
  }
  ESP_LOGW(TAG, "no conversion-ready flag after 500 ms, continuing anyway");
  return ESP_OK;
}

// -- Read ---------------------------------------------------------------------

esp_err_t ina228_read(ina228_reading_t *out) {
  int32_t  vbus_raw, vshunt_raw, current_raw;
  uint32_t power_raw;
  uint16_t temp_raw;
  int64_t  charge_raw, energy_raw;

  ESP_RETURN_ON_ERROR(reg_read20_signed(INA228_REG_VBUS, &vbus_raw), TAG, "vbus");
  ESP_RETURN_ON_ERROR(reg_read20_signed(INA228_REG_VSHUNT, &vshunt_raw), TAG, "vshunt");
  ESP_RETURN_ON_ERROR(reg_read20_signed(INA228_REG_CURRENT, &current_raw), TAG, "current");
  ESP_RETURN_ON_ERROR(reg_read24_unsigned(INA228_REG_POWER, &power_raw), TAG, "power");
  ESP_RETURN_ON_ERROR(reg_read16(INA228_REG_DIETEMP, &temp_raw), TAG, "dietemp");
  ESP_RETURN_ON_ERROR(reg_read40(INA228_REG_CHARGE, &charge_raw, true), TAG, "charge");
  ESP_RETURN_ON_ERROR(reg_read40(INA228_REG_ENERGY, &energy_raw, false), TAG, "energy");
  ESP_RETURN_ON_ERROR(reg_read16(INA228_REG_DIAG_ALRT, &out->diag), TAG, "diag");

  const float vshunt_lsb_v =
      (s_adcrange1 ? INA228_VSHUNT_LSB_NV_R1 : INA228_VSHUNT_LSB_NV_R0) * 1e-9f;

  out->bus_v     = vbus_raw * (float)(INA228_VBUS_LSB_UV * 1e-6);
  out->shunt_v   = vshunt_raw * vshunt_lsb_v;
  out->current_a = current_raw * s_current_lsb;
  out->power_w   = power_raw * 3.2f * s_current_lsb;          // S8.1.2 eq.5
  out->temp_c    = (int16_t)temp_raw * (float)(INA228_DIETEMP_LSB_MC / 1000.0);
  out->charge_c  = charge_raw * (double)s_current_lsb;         // C, LSB = CURRENT_LSB
  out->energy_j  = energy_raw * 16.0 * 3.2 * (double)s_current_lsb; // S8.1.2 eq.6

  return ESP_OK;
}
