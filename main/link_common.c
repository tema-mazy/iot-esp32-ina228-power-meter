#include "link.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void link_write_str(const char *s) {
  link_write((const uint8_t *)s, strlen(s));
}

void link_printf(const char *fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
    link_write((const uint8_t *)buf, n > (int)sizeof(buf) - 1 ? sizeof(buf) - 1 : n);
}
