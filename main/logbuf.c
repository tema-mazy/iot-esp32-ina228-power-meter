#include "logbuf.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char         s_lines[LOGBUF_LINES][LOGBUF_LINE_MAX];
static volatile int s_write;  // next slot, wraps
static volatile int s_total;  // lines ever written
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Deliberately does NOT chain to the original vprintf. With the console
// disabled there is nowhere for it to go, and on a shipping build any stray
// output on the AT link would corrupt the protocol.
static int log_capture(const char *fmt, va_list args) {
  char buf[LOGBUF_LINE_MAX];
  int n = vsnprintf(buf, sizeof(buf), fmt, args);

  int len = strnlen(buf, LOGBUF_LINE_MAX - 1);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = '\0';
  if (len == 0)
    return n;

  portENTER_CRITICAL_SAFE(&s_mux);
  memcpy(s_lines[s_write], buf, len + 1);
  s_write = (s_write + 1) % LOGBUF_LINES;
  s_total++;
  portEXIT_CRITICAL_SAFE(&s_mux);
  return n;
}

void logbuf_init(void) { esp_log_set_vprintf(log_capture); }

int logbuf_count(void) {
  portENTER_CRITICAL_SAFE(&s_mux);
  int t = s_total;
  portEXIT_CRITICAL_SAFE(&s_mux);
  return t < LOGBUF_LINES ? t : LOGBUF_LINES;
}

int logbuf_get(int index, char *out, size_t out_len) {
  int count = logbuf_count();
  if (index < 0 || index >= count)
    return -1;
  portENTER_CRITICAL_SAFE(&s_mux);
  int slot = (s_write - count + index + LOGBUF_LINES * 2) % LOGBUF_LINES;
  strlcpy(out, s_lines[slot], out_len);
  portEXIT_CRITICAL_SAFE(&s_mux);
  return 0;
}
