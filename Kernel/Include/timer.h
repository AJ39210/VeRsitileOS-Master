#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

#define PIT_FREQUENCY 100

bool timer_init(void);

uint64_t timer_get_ticks(void);
uint64_t timer_get_milliseconds(void);
void timer_systemd_watchdog_feed(uint32_t timeout_seconds);

#endif