// OTA over the AT link (plan S2.5):
//   host -> ATFW=<bytes>,<md5hex>
//   dev  <- OK                     device enters raw binary mode
//   host -> exactly <bytes> raw bytes
//   dev  <- OK                     MD5 verified, boot partition set, reboots
//        |  ERROR <code> <desc>    nothing changed, current image kept
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Inter-byte timeout. Generous: a stalled host should abort cleanly rather
// than leave the device stuck in raw mode.
#define OTA_RX_TIMEOUT_MS 5000

// Flow control. The host writes at USB speed; the device drains at roughly the
// speed it can write flash (~150 kB/s). The USB Serial/JTAG driver DROPS data
// on RX-ring overflow rather than applying backpressure, so the transfer is
// windowed: the device emits one '.' per chunk consumed, and the host sends
// the next chunk only after seeing it.
//
// OTA_CHUNK MUST be smaller than the link's RX buffer (see link_usb.c). The
// ACK paces the host BETWEEN chunks, not within one, so a chunk larger than
// the ring overflows it mid-chunk and the excess is discarded silently.
// Getting this wrong looks exactly like a stalled host: the device reports a
// timeout having received precisely rx_buffer_size bytes.
#define OTA_CHUNK      2048
#define OTA_CHUNK_ACK  '.'

// Validate before acknowledging. The OK reply commits the host to streaming
// binary immediately, so anything rejectable must be rejected first.
esp_err_t ota_check_size(size_t total);

esp_err_t ota_receive(size_t total, const uint8_t expect_md5[16]);

// Rollback health gate. A new image boots PENDING_VERIFY and is only kept once
// it proves it can both talk to the sensor and answer the host; anything less
// and the bootloader reverts. Stricter than "it booted", deliberately.
void ota_health_ina_ok(void);
void ota_health_at_served(void);
