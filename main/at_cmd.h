// AT command parser. One command per line, \r\n terminated, case-insensitive,
// one response line per command. Strictly request/response: the device never
// speaks first (plan S2).
#pragma once
#include "ina228.h"
#include <stdbool.h>

// Error codes, plan S2
#define AT_ERR_SYNTAX     1
#define AT_ERR_UNKNOWN    2
#define AT_ERR_PARAM      3
#define AT_ERR_NOCONFIG   4
#define AT_ERR_INA        5
#define AT_ERR_NVS        6
#define AT_ERR_OTA        7
#define AT_ERR_TIMEOUT    8
#define AT_ERR_CHECKSUM   9

void at_task(void *arg);

// Called by the gauge task each poll. Keeps the AT task off the I2C bus so a
// slow or wedged sensor can never delay a response (plan S5).
void at_publish(const ina228_reading_t *r, bool valid);
