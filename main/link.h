// Byte-stream transport for the AT protocol. The parser, OTA receiver and
// JSON emitter are written once against this interface; Kconfig selects the
// backend (plan S1.1). No #ifdef leaks above this line.
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Writes must never stall the gauge. A host that is not draining the FIFO is
// the normal state for a self-powered monitor with nothing plugged in, so a
// write that cannot complete is discarded rather than blocking (plan S1.2).
#define LINK_WRITE_TIMEOUT_MS 20

esp_err_t link_init(void);
int  link_read(uint8_t *buf, size_t len, uint32_t timeout_ms); // bytes, 0 = timeout
int  link_write(const uint8_t *buf, size_t len);               // bytes written
// Discard anything pending in RX. Required after an aborted OTA: leftover
// firmware payload would otherwise be parsed as AT commands, producing a
// flood of errors and desynchronising the protocol.
void link_flush_rx(void);

void link_write_str(const char *s);
void link_printf(const char *fmt, ...);
