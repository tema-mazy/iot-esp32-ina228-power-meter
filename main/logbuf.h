// Capture-only log ring buffer. ESP_LOG output is diverted here instead of a
// console, because the AT link owns the only serial port and must stay a
// strict request/response channel (plan S1.4). Logs surface only via ATL.
#pragma once
#include <stddef.h>

#define LOGBUF_LINES    64
#define LOGBUF_LINE_MAX 160

void logbuf_init(void);

// Non-destructive read: ATL is idempotent, so a dropped response does not
// lose the log.
int logbuf_count(void);
int logbuf_get(int index, char *out, size_t out_len);
