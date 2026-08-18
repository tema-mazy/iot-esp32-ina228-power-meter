// Single monochrome LED. All status is encoded in blink counts and timing,
// never colour (plan S2.6) - the LED type varies by board and the other two
// LEDs on the hardware are fixed power indicators.
#pragma once
#include <stdbool.h>

void led_init(void);
void led_set(bool on);
void led_blink(int count, int on_ms, int off_ms);

void led_ok(void);      // ~20 ms flicker: command answered OK
void led_error(void);   // 3 fast blinks: command answered ERROR
void led_soc(int soc);  // 2 fast blinks, then N slow blinks (N = soc/10)
